#include "UsefulAnalyzer.h"

using namespace clang;

namespace clad {

void UsefulAnalyzer::Analyze(const FunctionDecl* FD) {
  // llvm::errs() << "\nAnalyze";
  // Build the CFG (control-flow graph) of FD.
  clang::CFG::BuildOptions Options;
  m_CFG = clang::CFG::buildCFG(FD, FD->getBody(), &m_Context, Options);

  m_BlockData.resize(m_CFG->size());
  // Set current block ID to the ID of entry the block.
  CFGBlock* exit = &m_CFG->getExit();
  m_CurBlockID = exit->getBlockID();
  m_BlockData[m_CurBlockID] = createNewVarsData({});
  for (const VarDecl* i : m_UsefulDecls)
    m_BlockData[m_CurBlockID]->insert(i);
  // Add the entry block to the queue.
  m_CFGQueue.insert(m_CurBlockID);

  // Visit CFG blocks in the queue until it's empty.
  while (!m_CFGQueue.empty()) {
    auto IDIter = std::prev(m_CFGQueue.end());
    m_CurBlockID = *IDIter;
    m_CFGQueue.erase(IDIter);
    CFGBlock& nextBlock = *getCFGBlockByID(m_CurBlockID);
    AnalyzeCFGBlock(nextBlock);
  }
}

CFGBlock* UsefulAnalyzer::getCFGBlockByID(unsigned ID) {
  return *(m_CFG->begin() + ID);
}

bool UsefulAnalyzer::isUseful(const VarDecl* VD) const {
  const VarsData& curBranch = getCurBlockVarsData();
  return curBranch.find(VD) != curBranch.end();
}

void UsefulAnalyzer::copyVarToCurBlock(const clang::VarDecl* VD) {
  VarsData& curBranch = getCurBlockVarsData();
  curBranch.insert(VD);
}

void mergeVarsData(VarsData* targetData, VarsData* mergeData) {
  for (const clang::VarDecl* i : *mergeData)
    targetData->insert(i);
  for (const clang::VarDecl* i : *targetData)
    mergeData->insert(i);
}

void UsefulAnalyzer::AnalyzeCFGBlock(const CFGBlock& block) {
  // llvm::errs() << "\nAnalyzeCFGBlock";
  block.dump();

  for (auto ib = block.end(); ib != block.begin(); ib--) {
    if (ib->getKind() == clang::CFGElement::Statement) {
      // ib->dump();
      const clang::Stmt* S = ib->castAs<clang::CFGStmt>().getStmt();
      // The const_cast is inevitable, since there is no
      // ConstRecusiveASTVisitor.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      TraverseStmt(const_cast<clang::Stmt*>(S));
    }
  }
}

bool UsefulAnalyzer::VisitBinaryOperator(BinaryOperator* BinOp) {
  Expr* L = BinOp->getLHS();
  Expr* R = BinOp->getRHS();
  const auto opCode = BinOp->getOpcode();
  if (BinOp->isAssignmentOp()) {
    m_Useful = false;
    TraverseStmt(L);
    m_Marking = m_Useful;
    TraverseStmt(R);
    m_Marking = false;
  } else if (opCode == BO_Add || opCode == BO_Sub || opCode == BO_Mul ||
             opCode == BO_Div) {
    for (auto* subexpr : BinOp->children())
      if (!isa<BinaryOperator>(subexpr))
        TraverseStmt(subexpr);
  }
  return true;
}

bool UsefulAnalyzer::VisitDeclStmt(DeclStmt* DS) {
  for (Decl* D : DS->decls()) {
    if (!isa<VarDecl>(D))
      continue;
    if (auto* VD = cast<VarDecl>(D)) {
      if (isUseful(VD)) {
        m_Useful = true;
        m_Marking = true;
      }
      if (Expr* init = cast<VarDecl>(D)->getInit())
        TraverseStmt(init);
      m_Marking = false;
    }
  }
  return true;
}

bool UsefulAnalyzer::VisitReturnStmt(ReturnStmt* RS) {
  m_Useful = true;
  m_Marking = true;
  auto* rv = RS->getRetValue();
  TraverseStmt(rv);
  return true;
}

bool UsefulAnalyzer::VisitCallExpr(CallExpr* CE) {
  if (m_Useful)
    return true;
  FunctionDecl* FD = CE->getDirectCallee();
  m_UsefulFuncs.insert(FD);
  return true;
}

bool UsefulAnalyzer::VisitDeclRefExpr(DeclRefExpr* DRE) {
  auto* VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD)
    return true;

  if (isUseful(VD))
    m_Useful = true;

  if (m_Useful && m_Marking)
    copyVarToCurBlock(VD);

  return true;
}

} // namespace clad
