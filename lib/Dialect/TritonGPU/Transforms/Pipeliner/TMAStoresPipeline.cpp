#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/Transforms/PipeliningUtility.h"
#include "triton/Dialect/TritonGPU/Transforms/Schedule.h"
#include "triton/Dialect/TritonNvidiaGPU/IR/Dialect.h"
#include "triton/Dialect/TritonNvidiaGPU/Transforms/TMAUtilities.h"

using namespace mlir;
namespace tt = mlir::triton;
namespace ttg = mlir::triton::gpu;
namespace ttng = mlir::triton::nvidia_gpu;

struct TMAStore {
  Operation *op;
  mlir::TypedValue<tt::TensorDescType> desc;
  mlir::TypedValue<RankedTensorType> src;
};

static SmallVector<TMAStore> getTMAStores(scf::ForOp forOp) {
  SmallVector<TMAStore> tmaStores;

  forOp.getBody()->walk<mlir::WalkOrder::PreOrder>([&](Operation *op) {
    if (auto storeOp = dyn_cast<tt::DescriptorStoreLikeOpInterface>(op)) {
      tmaStores.push_back({storeOp, storeOp.getDesc(), storeOp.getSrc()});
      // Don't walk into nested loops.
    } else if (isa<scf::ForOp>(op)) {
      return WalkResult::skip();
    }
    return WalkResult::advance();
  });

  return tmaStores;
}

static Value createAlloc(scf::ForOp &forOp, const TMAStore &store) {
  OpBuilder builder(forOp);
  RankedTensorType ty = store.src.getType();
  auto encoding =
      triton::nvidia_gpu::getEncodingFromDescriptor(store.op, ty, store.desc);
  Attribute sharedMemorySpace =
      triton::gpu::SharedMemorySpaceAttr::get(ty.getContext());
  Type memdescType =
      ttg::MemDescType::get(ty.getShape(), ty.getElementType(), encoding,
                            sharedMemorySpace, /*mutableMemory*/ true);
  Value alloc =
      builder.create<ttg::LocalAllocOp>(store.op->getLoc(), memdescType);
  return alloc;
}

static void createTMAAsyncCopy(scf::ForOp forOp, const TMAStore &store,
                               Value alloc) {
  OpBuilder builder(store.op);
  Location loc = store.op->getLoc();
  RankedTensorType ty = store.src.getType();

  // Put wait before the local_store make the store truly async. We know
  // that we are the only user of the CopyLocalToGlobal.
  builder.create<ttng::TMAStoreWaitOp>(loc, 0);
  builder.create<ttg::LocalStoreOp>(loc, store.src, alloc);
  builder.create<ttng::FenceAsyncSharedOp>(loc, false);
  auto desc = store.desc;
  if (auto storeOp = dyn_cast<tt::DescriptorStoreOp>(store.op)) {
    auto indices = ttng::translateTMAIndices(
        builder, storeOp.getLoc(),
        storeOp.getDesc().getType().getBlockType().getEncoding(),
        storeOp.getIndices());
    builder.create<ttng::AsyncTMACopyLocalToGlobalOp>(
        loc, desc, storeOp.getIndices(), alloc);
  } else if (auto reduceOp = dyn_cast<tt::DescriptorReduceOp>(store.op)) {
    auto indices = ttng::translateTMAIndices(
        builder, reduceOp.getLoc(),
        reduceOp.getDesc().getType().getBlockType().getEncoding(),
        reduceOp.getIndices());
    builder.create<ttng::AsyncTMAReduceOp>(loc, reduceOp.getKind(), desc,
                                           reduceOp.getIndices(), alloc);
  } else {
    auto scatterOp = cast<tt::DescriptorScatterOp>(store.op);
    builder.create<ttng::AsyncTMAScatterOp>(loc, desc, scatterOp.getXOffsets(),
                                            scatterOp.getYOffset(), alloc);
  }

  store.op->erase();
}

static void lowerTMADescriptorCreation(scf::ForOp forOp) {
  // Use max_stage=3 to double buffer the descriptor.
  triton::CoarseSchedule schedule(3);
  triton::lowerTMADescriptors(forOp, schedule);
}

bool mlir::triton::pipelineTMAStores(scf::ForOp forOp) {
  SmallVector<TMAStore> tmaStores = getTMAStores(forOp);
  if (tmaStores.empty())
    return false;

  DenseMap<Operation *, Value> storeToAlloc;
  DenseMap<std::pair<ArrayRef<int64_t>, Type>, Value> allocs;
  for (const TMAStore &store : tmaStores) {
    // Reuse allocations for stores of the same shape and types. This allows
    // saving shared memory usage. It is valid since we have a wait 0 before
    // every local_store. We could pipeline more aggressively if we didn't
    // reuse but there is a tradeoff with shared memory usage.
    RankedTensorType srcTy = store.src.getType();
    auto key = std::make_pair(srcTy.getShape(), srcTy.getElementType());
    Value &alloc = allocs[key];
    if (!alloc) {
      alloc = createAlloc(forOp, store);
    }
    storeToAlloc[store.op] = alloc;
  }

  bool hasDeviceSideTMA = llvm::any_of(tmaStores, [](const TMAStore &store) {
    return !triton::isHostSideDescriptor(store.desc);
  });
  for (const TMAStore &store : tmaStores) {
    createTMAAsyncCopy(forOp, store, storeToAlloc[store.op]);
  }

  // Deallocate shared memory buffers.
  OpBuilder builder(forOp);
  builder.setInsertionPointAfter(forOp);
  builder.create<ttng::TMAStoreWaitOp>(forOp->getLoc(), 0);
  for (auto it : storeToAlloc) {
    builder.create<ttg::LocalDeallocOp>(forOp->getLoc(), it.second);
  }

  if (hasDeviceSideTMA) {
    // This is a bit coarse as it would multibuffer any descriptor in the loop
    // but it likely to not have a big impact.
    lowerTMADescriptorCreation(forOp);
  }
  return true;
}
