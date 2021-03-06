
#include "llvm/Support/TargetSelect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Vector/VectorOps.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "quantum_to_llvm.hpp"
#include "staq_parser.hpp"

using namespace mlir;
using namespace staq;

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv, "openqasm compiler\n");

  std::string lineText = R"#(OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
h q[0];
cx q[0], q[1];
CX q[1], q[0];
U(1.1,2.2,3.3) q[1];
rx(2.333) q[0];
gate foo q {
  ry(3.3) q;
}
foo q[0];
qreg r[3];
ccx r[0], r[1], r[2];
creg c[2];
measure q -> c;
)#";

  std::cout << "Original:\n" << lineText << "\n";
  // Parse the OpenQasm with Staq
  ast::ptr<ast::Program> prog;
  try {
    prog = parser::parse_string(lineText);
    transformations::inline_ast(*prog);
    transformations::desugar(*prog);
  } catch (std::exception &e) {
    std::stringstream ss;
    std::cout << e.what() << "\n";
  }

  std::cout << "After parsing:\n" << *prog << "\n";
  mlir::MLIRContext context;
  context.loadDialect<mlir::quantum::QuantumDialect, mlir::StandardOpsDialect,
                      mlir::vector::VectorDialect>();

  // Generate the MLIR using a Staq Visitor
  qasm_parser::StaqToMLIR visitor(context);
  visitor.visit(*prog);
  visitor.addReturn();
  auto module = visitor.module();

  std::cout << "MLIR + Quantum Dialect:\n";
  module->dump();

  // Create the PassManager for lowering to LLVM MLIR and run it
  mlir::PassManager pm(&context);
  pm.addPass(std::make_unique<qcor::QuantumToLLVMLoweringPass>());
  auto module_op = module.getOperation();
  if (mlir::failed(pm.run(module_op))) {
    std::cout << "Pass Manager Failed\n";
    return 1;
  }
  std::cout << "Lowered to LLVM MLIR Dialect:\n";
  module_op->dump();

  // Now lower MLIR to LLVM IR
  llvm::LLVMContext llvmContext;
  auto llvmModule = mlir::translateModuleToLLVMIR(module, llvmContext);
  std::cout << "Lowered to LLVM IR:\n";
  llvmModule->dump();

  // Optimize the LLVM IR
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  auto optPipeline = mlir::makeOptimizingTransformer(3, 0, nullptr);
  if (auto err = optPipeline(llvmModule.get())) {
    llvm::errs() << "Failed to optimize LLVM IR " << err << "\n";
    return -1;
  }
  std::cout << "Optimized LLVM IR:\n";
  llvmModule->dump();

  
  return 0;
}
