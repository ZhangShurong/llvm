//===-- AMDGPUTargetMachine.cpp - TargetMachine for hw codegen targets-----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// The AMDGPU target machine contains all of the hardware specific
/// information  needed to emit code for R600 and SI GPUs.
//
//===----------------------------------------------------------------------===//

#include "AMDGPUTargetMachine.h"
#include "AMDGPU.h"
#include "AMDGPUAliasAnalysis.h"
#include "AMDGPUCallLowering.h"
#include "AMDGPUInstructionSelector.h"
#include "AMDGPULegalizerInfo.h"
#include "AMDGPUMacroFusion.h"
#include "AMDGPUTargetObjectFile.h"
#include "AMDGPUTargetTransformInfo.h"
#include "GCNIterativeScheduler.h"
#include "GCNSchedStrategy.h"
#include "R600MachineScheduler.h"
#include "SIMachineFunctionInfo.h"
#include "SIMachineScheduler.h"
#include "TargetInfo/AMDGPUTargetInfo.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/MIRParser/MIParser.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Vectorize.h"
#include <memory>

using namespace llvm;

static cl::opt<bool> EnableR600StructurizeCFG(
  "r600-ir-structurize",
  cl::desc("Use StructurizeCFG IR pass"),
  cl::init(true));

static cl::opt<bool> EnableSROA(
  "amdgpu-sroa",
  cl::desc("Run SROA after promote alloca pass"),
  cl::ReallyHidden,
  cl::init(true));

static cl::opt<bool>
EnableEarlyIfConversion("amdgpu-early-ifcvt", cl::Hidden,
                        cl::desc("Run early if-conversion"),
                        cl::init(false));

static cl::opt<bool>
OptExecMaskPreRA("amdgpu-opt-exec-mask-pre-ra", cl::Hidden,
            cl::desc("Run pre-RA exec mask optimizations"),
            cl::init(true));

static cl::opt<bool> EnableR600IfConvert(
  "r600-if-convert",
  cl::desc("Use if conversion pass"),
  cl::ReallyHidden,
  cl::init(true));

// Option to disable vectorizer for tests.
static cl::opt<bool> EnableLoadStoreVectorizer(
  "amdgpu-load-store-vectorizer",
  cl::desc("Enable load store vectorizer"),
  cl::init(true),
  cl::Hidden);

// Option to control global loads scalarization
static cl::opt<bool> ScalarizeGlobal(
  "amdgpu-scalarize-global-loads",
  cl::desc("Enable global load scalarization"),
  cl::init(true),
  cl::Hidden);

// Option to run internalize pass.
static cl::opt<bool> InternalizeSymbols(
  "amdgpu-internalize-symbols",
  cl::desc("Enable elimination of non-kernel functions and unused globals"),
  cl::init(false),
  cl::Hidden);

// Option to inline all early.
static cl::opt<bool> EarlyInlineAll(
  "amdgpu-early-inline-all",
  cl::desc("Inline all functions early"),
  cl::init(false),
  cl::Hidden);

static cl::opt<bool> EnableSDWAPeephole(
  "amdgpu-sdwa-peephole",
  cl::desc("Enable SDWA peepholer"),
  cl::init(true));

static cl::opt<bool> EnableDPPCombine(
  "amdgpu-dpp-combine",
  cl::desc("Enable DPP combiner"),
  cl::init(true));

// Enable address space based alias analysis
static cl::opt<bool> EnableAMDGPUAliasAnalysis("enable-amdgpu-aa", cl::Hidden,
  cl::desc("Enable AMDGPU Alias Analysis"),
  cl::init(true));

// Option to run late CFG structurizer
static cl::opt<bool, true> LateCFGStructurize(
  "amdgpu-late-structurize",
  cl::desc("Enable late CFG structurization"),
  cl::location(AMDGPUTargetMachine::EnableLateStructurizeCFG),
  cl::Hidden);

static cl::opt<bool, true> EnableAMDGPUFunctionCallsOpt(
  "amdgpu-function-calls",
  cl::desc("Enable AMDGPU function call support"),
  cl::location(AMDGPUTargetMachine::EnableFunctionCalls),
  cl::init(true),
  cl::Hidden);

// Enable lib calls simplifications
static cl::opt<bool> EnableLibCallSimplify(
  "amdgpu-simplify-libcall",
  cl::desc("Enable amdgpu library simplifications"),
  cl::init(true),
  cl::Hidden);

static cl::opt<bool> EnableLowerKernelArguments(
  "amdgpu-ir-lower-kernel-arguments",
  cl::desc("Lower kernel argument loads in IR pass"),
  cl::init(true),
  cl::Hidden);

static cl::opt<bool> EnableRegReassign(
  "amdgpu-reassign-regs",
  cl::desc("Enable register reassign optimizations on gfx10+"),
  cl::init(true),
  cl::Hidden);

// Enable atomic optimization
static cl::opt<bool> EnableAtomicOptimizations(
  "amdgpu-atomic-optimizations",
  cl::desc("Enable atomic optimizations"),
  cl::init(false),
  cl::Hidden);

// Enable Mode register optimization
static cl::opt<bool> EnableSIModeRegisterPass(
  "amdgpu-mode-register",
  cl::desc("Enable mode register pass"),
  cl::init(true),
  cl::Hidden);

// Option is used in lit tests to prevent deadcoding of patterns inspected.
static cl::opt<bool>
EnableDCEInRA("amdgpu-dce-in-ra",
    cl::init(true), cl::Hidden,
    cl::desc("Enable machine DCE inside regalloc"));

static cl::opt<bool> EnableScalarIRPasses(
  "amdgpu-scalar-ir-passes",
  cl::desc("Enable scalar IR passes"),
  cl::init(true),
  cl::Hidden);

extern "C" void LLVMInitializeAMDGPUTarget() {
  // Register the target
  RegisterTargetMachine<R600TargetMachine> X(getTheAMDGPUTarget());
  RegisterTargetMachine<GCNTargetMachine> Y(getTheGCNTarget());

  PassRegistry *PR = PassRegistry::getPassRegistry();
  initializeR600ClauseMergePassPass(*PR);
  initializeR600ControlFlowFinalizerPass(*PR);
  initializeR600PacketizerPass(*PR);
  initializeR600ExpandSpecialInstrsPassPass(*PR);
  initializeR600VectorRegMergerPass(*PR);
  initializeGlobalISel(*PR);
  initializeAMDGPUDAGToDAGISelPass(*PR);
  initializeGCNDPPCombinePass(*PR);
  initializeSILowerI1CopiesPass(*PR);
  initializeSIFixSGPRCopiesPass(*PR);
  initializeSIFixVGPRCopiesPass(*PR);
  initializeSIFixupVectorISelPass(*PR);
  initializeSIFoldOperandsPass(*PR);
  initializeSIPeepholeSDWAPass(*PR);
  initializeSIShrinkInstructionsPass(*PR);
  initializeSIOptimizeExecMaskingPreRAPass(*PR);
  initializeSILoadStoreOptimizerPass(*PR);
  initializeAMDGPUFixFunctionBitcastsPass(*PR);
  initializeAMDGPUAlwaysInlinePass(*PR);
  initializeAMDGPUAnnotateKernelFeaturesPass(*PR);
  initializeAMDGPUAnnotateUniformValuesPass(*PR);
  initializeAMDGPUArgumentUsageInfoPass(*PR);
  initializeAMDGPUAtomicOptimizerPass(*PR);
  initializeAMDGPULowerKernelArgumentsPass(*PR);
  initializeAMDGPULowerKernelAttributesPass(*PR);
  initializeAMDGPULowerIntrinsicsPass(*PR);
  initializeAMDGPUOpenCLEnqueuedBlockLoweringPass(*PR);
  initializeAMDGPUPromoteAllocaPass(*PR);
  initializeAMDGPUCodeGenPreparePass(*PR);
  initializeAMDGPUPropagateAttributesEarlyPass(*PR);
  initializeAMDGPUPropagateAttributesLatePass(*PR);
  initializeAMDGPURewriteOutArgumentsPass(*PR);
  initializeAMDGPUUnifyMetadataPass(*PR);
  initializeSIAnnotateControlFlowPass(*PR);
  initializeSIInsertWaitcntsPass(*PR);
  initializeSIModeRegisterPass(*PR);
  initializeSIWholeQuadModePass(*PR);
  initializeSILowerControlFlowPass(*PR);
  initializeSIInsertSkipsPass(*PR);
  initializeSIMemoryLegalizerPass(*PR);
  initializeSIOptimizeExecMaskingPass(*PR);
  initializeSIPreAllocateWWMRegsPass(*PR);
  initializeSIFormMemoryClausesPass(*PR);
  initializeAMDGPUUnifyDivergentExitNodesPass(*PR);
  initializeAMDGPUAAWrapperPassPass(*PR);
  initializeAMDGPUExternalAAWrapperPass(*PR);
  initializeAMDGPUUseNativeCallsPass(*PR);
  initializeAMDGPUSimplifyLibCallsPass(*PR);
  initializeAMDGPUInlinerPass(*PR);
  initializeGCNRegBankReassignPass(*PR);
  initializeGCNNSAReassignPass(*PR);
}

static std::unique_ptr<TargetLoweringObjectFile> createTLOF(const Triple &TT) {
  return llvm::make_unique<AMDGPUTargetObjectFile>();
}

static ScheduleDAGInstrs *createR600MachineScheduler(MachineSchedContext *C) {
  return new ScheduleDAGMILive(C, llvm::make_unique<R600SchedStrategy>());
}

static ScheduleDAGInstrs *createSIMachineScheduler(MachineSchedContext *C) {
  return new SIScheduleDAGMI(C);
}

static ScheduleDAGInstrs *
createGCNMaxOccupancyMachineScheduler(MachineSchedContext *C) {
  ScheduleDAGMILive *DAG =
    new GCNScheduleDAGMILive(C, make_unique<GCNMaxOccupancySchedStrategy>(C));
  DAG->addMutation(createLoadClusterDAGMutation(DAG->TII, DAG->TRI));
  DAG->addMutation(createStoreClusterDAGMutation(DAG->TII, DAG->TRI));
  DAG->addMutation(createAMDGPUMacroFusionDAGMutation());
  return DAG;
}

static ScheduleDAGInstrs *
createIterativeGCNMaxOccupancyMachineScheduler(MachineSchedContext *C) {
  auto DAG = new GCNIterativeScheduler(C,
    GCNIterativeScheduler::SCHEDULE_LEGACYMAXOCCUPANCY);
  DAG->addMutation(createLoadClusterDAGMutation(DAG->TII, DAG->TRI));
  DAG->addMutation(createStoreClusterDAGMutation(DAG->TII, DAG->TRI));
  return DAG;
}

static ScheduleDAGInstrs *createMinRegScheduler(MachineSchedContext *C) {
  return new GCNIterativeScheduler(C,
    GCNIterativeScheduler::SCHEDULE_MINREGFORCED);
}

static ScheduleDAGInstrs *
createIterativeILPMachineScheduler(MachineSchedContext *C) {
  auto DAG = new GCNIterativeScheduler(C,
    GCNIterativeScheduler::SCHEDULE_ILP);
  DAG->addMutation(createLoadClusterDAGMutation(DAG->TII, DAG->TRI));
  DAG->addMutation(createStoreClusterDAGMutation(DAG->TII, DAG->TRI));
  DAG->addMutation(createAMDGPUMacroFusionDAGMutation());
  return DAG;
}

static MachineSchedRegistry
R600SchedRegistry("r600", "Run R600's custom scheduler",
                   createR600MachineScheduler);

static MachineSchedRegistry
SISchedRegistry("si", "Run SI's custom scheduler",
                createSIMachineScheduler);

static MachineSchedRegistry
GCNMaxOccupancySchedRegistry("gcn-max-occupancy",
                             "Run GCN scheduler to maximize occupancy",
                             createGCNMaxOccupancyMachineScheduler);

static MachineSchedRegistry
IterativeGCNMaxOccupancySchedRegistry("gcn-max-occupancy-experimental",
  "Run GCN scheduler to maximize occupancy (experimental)",
  createIterativeGCNMaxOccupancyMachineScheduler);

static MachineSchedRegistry
GCNMinRegSchedRegistry("gcn-minreg",
  "Run GCN iterative scheduler for minimal register usage (experimental)",
  createMinRegScheduler);

static MachineSchedRegistry
GCNILPSchedRegistry("gcn-ilp",
  "Run GCN iterative scheduler for ILP scheduling (experimental)",
  createIterativeILPMachineScheduler);

static StringRef computeDataLayout(const Triple &TT) {
  if (TT.getArch() == Triple::r600) {
    // 32-bit pointers.
      return "e-p:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128"
             "-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5";
  }

  // 32-bit private, local, and region pointers. 64-bit global, constant and
  // flat, non-integral buffer fat pointers.
    return "e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32"
         "-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128"
         "-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5"
         "-ni:7";
}

LLVM_READNONE
static StringRef getGPUOrDefault(const Triple &TT, StringRef GPU) {
  if (!GPU.empty())
    return GPU;

  // Need to default to a target with flat support for HSA.
  if (TT.getArch() == Triple::amdgcn)
    return TT.getOS() == Triple::AMDHSA ? "generic-hsa" : "generic";

  return "r600";
}

static Reloc::Model getEffectiveRelocModel(Optional<Reloc::Model> RM) {
  // The AMDGPU toolchain only supports generating shared objects, so we
  // must always use PIC.
  return Reloc::PIC_;
}

AMDGPUTargetMachine::AMDGPUTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         TargetOptions Options,
                                         Optional<Reloc::Model> RM,
                                         Optional<CodeModel::Model> CM,
                                         CodeGenOpt::Level OptLevel)
    : LLVMTargetMachine(T, computeDataLayout(TT), TT, getGPUOrDefault(TT, CPU),
                        FS, Options, getEffectiveRelocModel(RM),
                        getEffectiveCodeModel(CM, CodeModel::Small), OptLevel),
      TLOF(createTLOF(getTargetTriple())) {
  initAsmInfo();
}

bool AMDGPUTargetMachine::EnableLateStructurizeCFG = false;
bool AMDGPUTargetMachine::EnableFunctionCalls = false;

AMDGPUTargetMachine::~AMDGPUTargetMachine() = default;

StringRef AMDGPUTargetMachine::getGPUName(const Function &F) const {
  Attribute GPUAttr = F.getFnAttribute("target-cpu");
  return GPUAttr.hasAttribute(Attribute::None) ?
    getTargetCPU() : GPUAttr.getValueAsString();
}

StringRef AMDGPUTargetMachine::getFeatureString(const Function &F) const {
  Attribute FSAttr = F.getFnAttribute("target-features");

  return FSAttr.hasAttribute(Attribute::None) ?
    getTargetFeatureString() :
    FSAttr.getValueAsString();
}

/// Predicate for Internalize pass.
static bool mustPreserveGV(const GlobalValue &GV) {
  if (const Function *F = dyn_cast<Function>(&GV))
    return F->isDeclaration() || AMDGPU::isEntryFunctionCC(F->getCallingConv());

  return !GV.use_empty();
}

void AMDGPUTargetMachine::adjustPassManager(PassManagerBuilder &Builder) {
  Builder.DivergentTarget = true;

  bool EnableOpt = getOptLevel() > CodeGenOpt::None;
  bool Internalize = InternalizeSymbols;
  bool EarlyInline = EarlyInlineAll && EnableOpt && !EnableFunctionCalls;
  bool AMDGPUAA = EnableAMDGPUAliasAnalysis && EnableOpt;
  bool LibCallSimplify = EnableLibCallSimplify && EnableOpt;

  if (EnableFunctionCalls) {
    delete Builder.Inliner;
    Builder.Inliner = createAMDGPUFunctionInliningPass();
  }

  Builder.addExtension(
    PassManagerBuilder::EP_ModuleOptimizerEarly,
    [Internalize, EarlyInline, AMDGPUAA, this](const PassManagerBuilder &,
                                               legacy::PassManagerBase &PM) {
      if (AMDGPUAA) {
        PM.add(createAMDGPUAAWrapperPass());
        PM.add(createAMDGPUExternalAAWrapperPass());
      }
      PM.add(createAMDGPUUnifyMetadataPass());
      PM.add(createAMDGPUPropagateAttributesLatePass(this));
      if (Internalize) {
        PM.add(createInternalizePass(mustPreserveGV));
        PM.add(createGlobalDCEPass());
      }
      if (EarlyInline)
        PM.add(createAMDGPUAlwaysInlinePass(false));
  });

  const auto &Opt = Options;
  Builder.addExtension(
    PassManagerBuilder::EP_EarlyAsPossible,
    [AMDGPUAA, LibCallSimplify, &Opt, this](const PassManagerBuilder &,
                                            legacy::PassManagerBase &PM) {
      if (AMDGPUAA) {
        PM.add(createAMDGPUAAWrapperPass());
        PM.add(createAMDGPUExternalAAWrapperPass());
      }
      PM.add(llvm::createAMDGPUPropagateAttributesEarlyPass(this));
      PM.add(llvm::createAMDGPUUseNativeCallsPass());
      if (LibCallSimplify)
        PM.add(llvm::createAMDGPUSimplifyLibCallsPass(Opt, this));
  });

  Builder.addExtension(
    PassManagerBuilder::EP_CGSCCOptimizerLate,
    [](const PassManagerBuilder &, legacy::PassManagerBase &PM) {
      // Add infer address spaces pass to the opt pipeline after inlining
      // but before SROA to increase SROA opportunities.
      PM.add(createInferAddressSpacesPass());

      // This should run after inlining to have any chance of doing anything,
      // and before other cleanup optimizations.
      PM.add(createAMDGPULowerKernelAttributesPass());
  });
}

//===----------------------------------------------------------------------===//
// R600 Target Machine (R600 -> Cayman)
//===----------------------------------------------------------------------===//

R600TargetMachine::R600TargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     TargetOptions Options,
                                     Optional<Reloc::Model> RM,
                                     Optional<CodeModel::Model> CM,
                                     CodeGenOpt::Level OL, bool JIT)
    : AMDGPUTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL) {
  setRequiresStructuredCFG(true);

  // Override the default since calls aren't supported for r600.
  if (EnableFunctionCalls &&
      EnableAMDGPUFunctionCallsOpt.getNumOccurrences() == 0)
    EnableFunctionCalls = false;
}

const R600Subtarget *R600TargetMachine::getSubtargetImpl(
  const Function &F) const {
  StringRef GPU = getGPUName(F);
  StringRef FS = getFeatureString(F);

  SmallString<128> SubtargetKey(GPU);
  SubtargetKey.append(FS);

  auto &I = SubtargetMap[SubtargetKey];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    I = llvm::make_unique<R600Subtarget>(TargetTriple, GPU, FS, *this);
  }

  return I.get();
}

TargetTransformInfo
R600TargetMachine::getTargetTransformInfo(const Function &F) {
  return TargetTransformInfo(R600TTIImpl(this, F));
}

//===----------------------------------------------------------------------===//
// GCN Target Machine (SI+)
//===----------------------------------------------------------------------===//

GCNTargetMachine::GCNTargetMachine(const Target &T, const Triple &TT,
                                   StringRef CPU, StringRef FS,
                                   TargetOptions Options,
                                   Optional<Reloc::Model> RM,
                                   Optional<CodeModel::Model> CM,
                                   CodeGenOpt::Level OL, bool JIT)
    : AMDGPUTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL) {}

const GCNSubtarget *GCNTargetMachine::getSubtargetImpl(const Function &F) const {
  StringRef GPU = getGPUName(F);
  StringRef FS = getFeatureString(F);

  SmallString<128> SubtargetKey(GPU);
  SubtargetKey.append(FS);

  auto &I = SubtargetMap[SubtargetKey];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    I = llvm::make_unique<GCNSubtarget>(TargetTriple, GPU, FS, *this);
  }

  I->setScalarizeGlobalBehavior(ScalarizeGlobal);

  return I.get();
}

TargetTransformInfo
GCNTargetMachine::getTargetTransformInfo(const Function &F) {
  return TargetTransformInfo(GCNTTIImpl(this, F));
}

//===----------------------------------------------------------------------===//
// AMDGPU Pass Setup
//===----------------------------------------------------------------------===//

namespace {

class AMDGPUPassConfig : public TargetPassConfig {
public:
  AMDGPUPassConfig(LLVMTargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {
    // Exceptions and StackMaps are not supported, so these passes will never do
    // anything.
    disablePass(&StackMapLivenessID);
    disablePass(&FuncletLayoutID);
  }

  AMDGPUTargetMachine &getAMDGPUTargetMachine() const {
    return getTM<AMDGPUTargetMachine>();
  }

  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override {
    ScheduleDAGMILive *DAG = createGenericSchedLive(C);
    DAG->addMutation(createLoadClusterDAGMutation(DAG->TII, DAG->TRI));
    DAG->addMutation(createStoreClusterDAGMutation(DAG->TII, DAG->TRI));
    return DAG;
  }

  void addEarlyCSEOrGVNPass();
  void addStraightLineScalarOptimizationPasses();
  void addIRPasses() override;
  void addCodeGenPrepare() override;
  bool addPreISel() override;
  bool addInstSelector() override;
  bool addGCPasses() override;

  std::unique_ptr<CSEConfigBase> getCSEConfig() const override;
};

std::unique_ptr<CSEConfigBase> AMDGPUPassConfig::getCSEConfig() const {
  return getStandardCSEConfigForOpt(TM->getOptLevel());
}

class R600PassConfig final : public AMDGPUPassConfig {
public:
  R600PassConfig(LLVMTargetMachine &TM, PassManagerBase &PM)
    : AMDGPUPassConfig(TM, PM) {}

  ScheduleDAGInstrs *createMachineScheduler(
    MachineSchedContext *C) const override {
    return createR600MachineScheduler(C);
  }

  bool addPreISel() override;
  bool addInstSelector() override;
  void addPreRegAlloc() override;
  void addPreSched2() override;
  void addPreEmitPass() override;
};

class GCNPassConfig final : public AMDGPUPassConfig {
public:
  GCNPassConfig(LLVMTargetMachine &TM, PassManagerBase &PM)
    : AMDGPUPassConfig(TM, PM) {
    // It is necessary to know the register usage of the entire call graph.  We
    // allow calls without EnableAMDGPUFunctionCalls if they are marked
    // noinline, so this is always required.
    setRequiresCodeGenSCCOrder(true);
  }

  GCNTargetMachine &getGCNTargetMachine() const {
    return getTM<GCNTargetMachine>();
  }

  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override;

  bool addPreISel() override;
  void addMachineSSAOptimization() override;
  bool addILPOpts() override;
  bool addInstSelector() override;
  bool addIRTranslator() override;
  bool addLegalizeMachineIR() override;
  bool addRegBankSelect() override;
  bool addGlobalInstructionSelect() override;
  void addFastRegAlloc() override;
  void addOptimizedRegAlloc() override;
  void addPreRegAlloc() override;
  bool addPreRewrite() override;
  void addPostRegAlloc() override;
  void addPreSched2() override;
  void addPreEmitPass() override;
};

} // end anonymous namespace

void AMDGPUPassConfig::addEarlyCSEOrGVNPass() {
  if (getOptLevel() == CodeGenOpt::Aggressive)
    addPass(createGVNPass());
  else
    addPass(createEarlyCSEPass());
}

void AMDGPUPassConfig::addStraightLineScalarOptimizationPasses() {
  addPass(createLICMPass());
  addPass(createSeparateConstOffsetFromGEPPass());
  addPass(createSpeculativeExecutionPass());
  // ReassociateGEPs exposes more opportunites for SLSR. See
  // the example in reassociate-geps-and-slsr.ll.
  addPass(createStraightLineStrengthReducePass());
  // SeparateConstOffsetFromGEP and SLSR creates common expressions which GVN or
  // EarlyCSE can reuse.
  addEarlyCSEOrGVNPass();
  // Run NaryReassociate after EarlyCSE/GVN to be more effective.
  addPass(createNaryReassociatePass());
  // NaryReassociate on GEPs creates redundant common expressions, so run
  // EarlyCSE after it.
  addPass(createEarlyCSEPass());
}

void AMDGPUPassConfig::addIRPasses() {
  const AMDGPUTargetMachine &TM = getAMDGPUTargetMachine();

  // There is no reason to run these.
  disablePass(&StackMapLivenessID);
  disablePass(&FuncletLayoutID);
  disablePass(&PatchableFunctionID);

  // This must occur before inlining, as the inliner will not look through
  // bitcast calls.
  addPass(createAMDGPUFixFunctionBitcastsPass());

  // A call to propagate attributes pass in the backend in case opt was not run.
  addPass(createAMDGPUPropagateAttributesEarlyPass(&TM));

  addPass(createAtomicExpandPass());


  addPass(createAMDGPULowerIntrinsicsPass());

  // Function calls are not supported, so make sure we inline everything.
  addPass(createAMDGPUAlwaysInlinePass());
  addPass(createAlwaysInlinerLegacyPass());
  // We need to add the barrier noop pass, otherwise adding the function
  // inlining pass will cause all of the PassConfigs passes to be run
  // one function at a time, which means if we have a nodule with two
  // functions, then we will generate code for the first function
  // without ever running any passes on the second.
  addPass(createBarrierNoopPass());

  if (TM.getTargetTriple().getArch() == Triple::amdgcn) {
    // TODO: May want to move later or split into an early and late one.

    addPass(createAMDGPUCodeGenPreparePass());
  }

  // Handle uses of OpenCL image2d_t, image3d_t and sampler_t arguments.
  if (TM.getTargetTriple().getArch() == Triple::r600)
    addPass(createR600OpenCLImageTypeLoweringPass());

  // Replace OpenCL enqueued block function pointers with global variables.
  addPass(createAMDGPUOpenCLEnqueuedBlockLoweringPass());

  if (TM.getOptLevel() > CodeGenOpt::None) {
    addPass(createInferAddressSpacesPass());
    addPass(createAMDGPUPromoteAlloca());

    if (EnableSROA)
      addPass(createSROAPass());

    if (EnableScalarIRPasses)
      addStraightLineScalarOptimizationPasses();

    if (EnableAMDGPUAliasAnalysis) {
      addPass(createAMDGPUAAWrapperPass());
      addPass(createExternalAAWrapperPass([](Pass &P, Function &,
                                             AAResults &AAR) {
        if (auto *WrapperPass = P.getAnalysisIfAvailable<AMDGPUAAWrapperPass>())
          AAR.addAAResult(WrapperPass->getResult());
        }));
    }
  }

  TargetPassConfig::addIRPasses();

  // EarlyCSE is not always strong enough to clean up what LSR produces. For
  // example, GVN can combine
  //
  //   %0 = add %a, %b
  //   %1 = add %b, %a
  //
  // and
  //
  //   %0 = shl nsw %a, 2
  //   %1 = shl %a, 2
  //
  // but EarlyCSE can do neither of them.
  if (getOptLevel() != CodeGenOpt::None && EnableScalarIRPasses)
    addEarlyCSEOrGVNPass();
}

void AMDGPUPassConfig::addCodeGenPrepare() {
  if (TM->getTargetTriple().getArch() == Triple::amdgcn)
    addPass(createAMDGPUAnnotateKernelFeaturesPass());

  if (TM->getTargetTriple().getArch() == Triple::amdgcn &&
      EnableLowerKernelArguments)
    addPass(createAMDGPULowerKernelArgumentsPass());

  TargetPassConfig::addCodeGenPrepare();

  if (EnableLoadStoreVectorizer)
    addPass(createLoadStoreVectorizerPass());
}

bool AMDGPUPassConfig::addPreISel() {
  addPass(createLowerSwitchPass());
  addPass(createFlattenCFGPass());
  return false;
}

bool AMDGPUPassConfig::addInstSelector() {
  // Defer the verifier until FinalizeISel.
  addPass(createAMDGPUISelDag(&getAMDGPUTargetMachine(), getOptLevel()), false);
  return false;
}

bool AMDGPUPassConfig::addGCPasses() {
  // Do nothing. GC is not supported.
  return false;
}

//===----------------------------------------------------------------------===//
// R600 Pass Setup
//===----------------------------------------------------------------------===//

bool R600PassConfig::addPreISel() {
  AMDGPUPassConfig::addPreISel();

  if (EnableR600StructurizeCFG)
    addPass(createStructurizeCFGPass());
  return false;
}

bool R600PassConfig::addInstSelector() {
  addPass(createR600ISelDag(&getAMDGPUTargetMachine(), getOptLevel()));
  return false;
}

void R600PassConfig::addPreRegAlloc() {
  addPass(createR600VectorRegMerger());
}

void R600PassConfig::addPreSched2() {
  addPass(createR600EmitClauseMarkers(), false);
  if (EnableR600IfConvert)
    addPass(&IfConverterID, false);
  addPass(createR600ClauseMergePass(), false);
}

void R600PassConfig::addPreEmitPass() {
  addPass(createAMDGPUCFGStructurizerPass(), false);
  addPass(createR600ExpandSpecialInstrsPass(), false);
  addPass(&FinalizeMachineBundlesID, false);
  addPass(createR600Packetizer(), false);
  addPass(createR600ControlFlowFinalizer(), false);
}

TargetPassConfig *R600TargetMachine::createPassConfig(PassManagerBase &PM) {
  return new R600PassConfig(*this, PM);
}

//===----------------------------------------------------------------------===//
// GCN Pass Setup
//===----------------------------------------------------------------------===//

ScheduleDAGInstrs *GCNPassConfig::createMachineScheduler(
  MachineSchedContext *C) const {
  const GCNSubtarget &ST = C->MF->getSubtarget<GCNSubtarget>();
  if (ST.enableSIScheduler())
    return createSIMachineScheduler(C);
  return createGCNMaxOccupancyMachineScheduler(C);
}

bool GCNPassConfig::addPreISel() {
  AMDGPUPassConfig::addPreISel();

  if (EnableAtomicOptimizations) {
    addPass(createAMDGPUAtomicOptimizerPass());
  }

  // FIXME: We need to run a pass to propagate the attributes when calls are
  // supported.

  // Merge divergent exit nodes. StructurizeCFG won't recognize the multi-exit
  // regions formed by them.
  addPass(&AMDGPUUnifyDivergentExitNodesID);
  if (!LateCFGStructurize) {
    addPass(createStructurizeCFGPass(true)); // true -> SkipUniformRegions
  }
  addPass(createSinkingPass());
  addPass(createAMDGPUAnnotateUniformValues());
  if (!LateCFGStructurize) {
    addPass(createSIAnnotateControlFlowPass());
  }
  addPass(createLCSSAPass());

  return false;
}

void GCNPassConfig::addMachineSSAOptimization() {
  TargetPassConfig::addMachineSSAOptimization();

  // We want to fold operands after PeepholeOptimizer has run (or as part of
  // it), because it will eliminate extra copies making it easier to fold the
  // real source operand. We want to eliminate dead instructions after, so that
  // we see fewer uses of the copies. We then need to clean up the dead
  // instructions leftover after the operands are folded as well.
  //
  // XXX - Can we get away without running DeadMachineInstructionElim again?
  addPass(&SIFoldOperandsID);
  if (EnableDPPCombine)
    addPass(&GCNDPPCombineID);
  addPass(&DeadMachineInstructionElimID);
  addPass(&SILoadStoreOptimizerID);
  if (EnableSDWAPeephole) {
    addPass(&SIPeepholeSDWAID);
    addPass(&EarlyMachineLICMID);
    addPass(&MachineCSEID);
    addPass(&SIFoldOperandsID);
    addPass(&DeadMachineInstructionElimID);
  }
  addPass(createSIShrinkInstructionsPass());
}

bool GCNPassConfig::addILPOpts() {
  if (EnableEarlyIfConversion)
    addPass(&EarlyIfConverterID);

  TargetPassConfig::addILPOpts();
  return false;
}

bool GCNPassConfig::addInstSelector() {
  AMDGPUPassConfig::addInstSelector();
  addPass(&SIFixSGPRCopiesID);
  addPass(createSILowerI1CopiesPass());
  addPass(createSIFixupVectorISelPass());
  addPass(createSIAddIMGInitPass());
  return false;
}

bool GCNPassConfig::addIRTranslator() {
  addPass(new IRTranslator());
  return false;
}

bool GCNPassConfig::addLegalizeMachineIR() {
  addPass(new Legalizer());
  return false;
}

bool GCNPassConfig::addRegBankSelect() {
  addPass(new RegBankSelect());
  return false;
}

bool GCNPassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelect());
  return false;
}

void GCNPassConfig::addPreRegAlloc() {
  if (LateCFGStructurize) {
    addPass(createAMDGPUMachineCFGStructurizerPass());
  }
  addPass(createSIWholeQuadModePass());
}

void GCNPassConfig::addFastRegAlloc() {
  // FIXME: We have to disable the verifier here because of PHIElimination +
  // TwoAddressInstructions disabling it.

  // This must be run immediately after phi elimination and before
  // TwoAddressInstructions, otherwise the processing of the tied operand of
  // SI_ELSE will introduce a copy of the tied operand source after the else.
  insertPass(&PHIEliminationID, &SILowerControlFlowID, false);

  // This must be run just after RegisterCoalescing.
  insertPass(&RegisterCoalescerID, &SIPreAllocateWWMRegsID, false);

  TargetPassConfig::addFastRegAlloc();
}

void GCNPassConfig::addOptimizedRegAlloc() {
  if (OptExecMaskPreRA) {
    insertPass(&MachineSchedulerID, &SIOptimizeExecMaskingPreRAID);
    insertPass(&SIOptimizeExecMaskingPreRAID, &SIFormMemoryClausesID);
  } else {
    insertPass(&MachineSchedulerID, &SIFormMemoryClausesID);
  }

  // This must be run immediately after phi elimination and before
  // TwoAddressInstructions, otherwise the processing of the tied operand of
  // SI_ELSE will introduce a copy of the tied operand source after the else.
  insertPass(&PHIEliminationID, &SILowerControlFlowID, false);

  // This must be run just after RegisterCoalescing.
  insertPass(&RegisterCoalescerID, &SIPreAllocateWWMRegsID, false);

  if (EnableDCEInRA)
    insertPass(&RenameIndependentSubregsID, &DeadMachineInstructionElimID);

  TargetPassConfig::addOptimizedRegAlloc();
}

bool GCNPassConfig::addPreRewrite() {
  if (EnableRegReassign) {
    addPass(&GCNNSAReassignID);
    addPass(&GCNRegBankReassignID);
  }
  return true;
}

void GCNPassConfig::addPostRegAlloc() {
  addPass(&SIFixVGPRCopiesID);
  if (getOptLevel() > CodeGenOpt::None)
    addPass(&SIOptimizeExecMaskingID);
  TargetPassConfig::addPostRegAlloc();
}

void GCNPassConfig::addPreSched2() {
}

void GCNPassConfig::addPreEmitPass() {
  addPass(createSIMemoryLegalizerPass());
  addPass(createSIInsertWaitcntsPass());
  addPass(createSIShrinkInstructionsPass());
  addPass(createSIModeRegisterPass());

  // The hazard recognizer that runs as part of the post-ra scheduler does not
  // guarantee to be able handle all hazards correctly. This is because if there
  // are multiple scheduling regions in a basic block, the regions are scheduled
  // bottom up, so when we begin to schedule a region we don't know what
  // instructions were emitted directly before it.
  //
  // Here we add a stand-alone hazard recognizer pass which can handle all
  // cases.
  //
  // FIXME: This stand-alone pass will emit indiv. S_NOP 0, as needed. It would
  // be better for it to emit S_NOP <N> when possible.
  addPass(&PostRAHazardRecognizerID);

  addPass(&SIInsertSkipsPassID);
  addPass(&BranchRelaxationPassID);
}

TargetPassConfig *GCNTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new GCNPassConfig(*this, PM);
}

yaml::MachineFunctionInfo *GCNTargetMachine::createDefaultFuncInfoYAML() const {
  return new yaml::SIMachineFunctionInfo();
}

yaml::MachineFunctionInfo *
GCNTargetMachine::convertFuncInfoToYAML(const MachineFunction &MF) const {
  const SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();
  return new yaml::SIMachineFunctionInfo(*MFI,
                                         *MF.getSubtarget().getRegisterInfo());
}

bool GCNTargetMachine::parseMachineFunctionInfo(
    const yaml::MachineFunctionInfo &MFI_, PerFunctionMIParsingState &PFS,
    SMDiagnostic &Error, SMRange &SourceRange) const {
  const yaml::SIMachineFunctionInfo &YamlMFI =
      reinterpret_cast<const yaml::SIMachineFunctionInfo &>(MFI_);
  MachineFunction &MF = PFS.MF;
  SIMachineFunctionInfo *MFI = MF.getInfo<SIMachineFunctionInfo>();

  MFI->initializeBaseYamlFields(YamlMFI);

  auto parseRegister = [&](const yaml::StringValue &RegName, unsigned &RegVal) {
    if (parseNamedRegisterReference(PFS, RegVal, RegName.Value, Error)) {
      SourceRange = RegName.SourceRange;
      return true;
    }

    return false;
  };

  auto diagnoseRegisterClass = [&](const yaml::StringValue &RegName) {
    // Create a diagnostic for a the register string literal.
    const MemoryBuffer &Buffer =
        *PFS.SM->getMemoryBuffer(PFS.SM->getMainFileID());
    Error = SMDiagnostic(*PFS.SM, SMLoc(), Buffer.getBufferIdentifier(), 1,
                         RegName.Value.size(), SourceMgr::DK_Error,
                         "incorrect register class for field", RegName.Value,
                         None, None);
    SourceRange = RegName.SourceRange;
    return true;
  };

  if (parseRegister(YamlMFI.ScratchRSrcReg, MFI->ScratchRSrcReg) ||
      parseRegister(YamlMFI.ScratchWaveOffsetReg, MFI->ScratchWaveOffsetReg) ||
      parseRegister(YamlMFI.FrameOffsetReg, MFI->FrameOffsetReg) ||
      parseRegister(YamlMFI.StackPtrOffsetReg, MFI->StackPtrOffsetReg))
    return true;

  if (MFI->ScratchRSrcReg != AMDGPU::PRIVATE_RSRC_REG &&
      !AMDGPU::SReg_128RegClass.contains(MFI->ScratchRSrcReg)) {
    return diagnoseRegisterClass(YamlMFI.ScratchRSrcReg);
  }

  if (MFI->ScratchWaveOffsetReg != AMDGPU::SCRATCH_WAVE_OFFSET_REG &&
      !AMDGPU::SGPR_32RegClass.contains(MFI->ScratchWaveOffsetReg)) {
    return diagnoseRegisterClass(YamlMFI.ScratchWaveOffsetReg);
  }

  if (MFI->FrameOffsetReg != AMDGPU::FP_REG &&
      !AMDGPU::SGPR_32RegClass.contains(MFI->FrameOffsetReg)) {
    return diagnoseRegisterClass(YamlMFI.FrameOffsetReg);
  }

  if (MFI->StackPtrOffsetReg != AMDGPU::SP_REG &&
      !AMDGPU::SGPR_32RegClass.contains(MFI->StackPtrOffsetReg)) {
    return diagnoseRegisterClass(YamlMFI.StackPtrOffsetReg);
  }

  auto parseAndCheckArgument = [&](const Optional<yaml::SIArgument> &A,
                                   const TargetRegisterClass &RC,
                                   ArgDescriptor &Arg) {
    // Skip parsing if it's not present.
    if (!A)
      return false;

    if (A->IsRegister) {
      unsigned Reg;
      if (parseNamedRegisterReference(PFS, Reg, A->RegisterName.Value,
                                      Error)) {
        SourceRange = A->RegisterName.SourceRange;
        return true;
      }
      if (!RC.contains(Reg))
        return diagnoseRegisterClass(A->RegisterName);
      Arg = ArgDescriptor::createRegister(Reg);
    } else
      Arg = ArgDescriptor::createStack(A->StackOffset);
    // Check and apply the optional mask.
    if (A->Mask)
      Arg = ArgDescriptor::createArg(Arg, A->Mask.getValue());

    return false;
  };

  if (YamlMFI.ArgInfo &&
      (parseAndCheckArgument(YamlMFI.ArgInfo->PrivateSegmentBuffer,
                             AMDGPU::SReg_128RegClass,
                             MFI->ArgInfo.PrivateSegmentBuffer) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->DispatchPtr,
                             AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.DispatchPtr) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->QueuePtr, AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.QueuePtr) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->KernargSegmentPtr,
                             AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.KernargSegmentPtr) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->DispatchID,
                             AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.DispatchID) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->FlatScratchInit,
                             AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.FlatScratchInit) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->PrivateSegmentSize,
                             AMDGPU::SGPR_32RegClass,
                             MFI->ArgInfo.PrivateSegmentSize) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkGroupIDX,
                             AMDGPU::SGPR_32RegClass,
                             MFI->ArgInfo.WorkGroupIDX) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkGroupIDY,
                             AMDGPU::SGPR_32RegClass,
                             MFI->ArgInfo.WorkGroupIDY) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkGroupIDZ,
                             AMDGPU::SGPR_32RegClass,
                             MFI->ArgInfo.WorkGroupIDZ) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkGroupInfo,
                             AMDGPU::SGPR_32RegClass,
                             MFI->ArgInfo.WorkGroupInfo) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->PrivateSegmentWaveByteOffset,
                             AMDGPU::SGPR_32RegClass,
                             MFI->ArgInfo.PrivateSegmentWaveByteOffset) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->ImplicitArgPtr,
                             AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.ImplicitArgPtr) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->ImplicitBufferPtr,
                             AMDGPU::SReg_64RegClass,
                             MFI->ArgInfo.ImplicitBufferPtr) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkItemIDX,
                             AMDGPU::VGPR_32RegClass,
                             MFI->ArgInfo.WorkItemIDX) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkItemIDY,
                             AMDGPU::VGPR_32RegClass,
                             MFI->ArgInfo.WorkItemIDY) ||
       parseAndCheckArgument(YamlMFI.ArgInfo->WorkItemIDZ,
                             AMDGPU::VGPR_32RegClass,
                             MFI->ArgInfo.WorkItemIDZ)))
    return true;

  return false;
}
