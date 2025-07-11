//--------------------------------------------------------------------*- C++ -*-
// clad - the C++ Clang-based Automatic Differentiator
// version: $Id: ClangPlugin.cpp 7 2013-06-01 22:48:03Z v.g.vassilev@gmail.com $
// author:  Vassil Vassilev <vvasilev-at-cern.ch>
//------------------------------------------------------------------------------

#ifndef CLAD_VISITOR_BASE_H
#define CLAD_VISITOR_BASE_H

#include "Compatibility.h"
#include "DerivativeBuilder.h"
#include "clad/Differentiator/CladUtils.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/Type.h"
#include "clang/Basic/OperatorKinds.h"
#include "clang/Sema/Ownership.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/PrettyStackTrace.h"

#include <array>
#include <stack>
#include <unordered_map>

namespace clang {
class NestedNameSpecifier;
} // namespace clang

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace clad {
  class MultiplexExternalRMVSource;
  /// A class that represents the result of Visit of ForwardModeVisitor.
  /// Stmt() allows to access the original (cloned) Stmt and Stmt_dx() allows
  /// to access its derivative (if exists, otherwise null). If Visit produces
  /// other (intermediate) statements, they are output to the current block.
  class StmtDiff {
  private:
    std::array<clang::Stmt*, 2> data;
    clang::Stmt* m_ValueForRevSweep;

  public:
    StmtDiff(clang::Stmt* orig = nullptr, clang::Stmt* diff = nullptr,
             clang::Stmt* valueForRevSweep = nullptr)
        : m_ValueForRevSweep(valueForRevSweep) {
      data[1] = orig;
      data[0] = diff;
    }

    clang::Stmt* getStmt() { return data[1]; }
    clang::Stmt* getStmt_dx() { return data[0]; }
    clang::Expr* getExpr() {
      return llvm::cast_or_null<clang::Expr>(getStmt());
    }
    clang::Expr* getExpr_dx() {
      return llvm::cast_or_null<clang::Expr>(getStmt_dx());
    }

    void updateStmt(clang::Stmt* S) { data[1] = S; }
    void updateStmtDx(clang::Stmt* S) { data[0] = S; }
    // Stmt_dx goes first!
    std::array<clang::Stmt*, 2>& getBothStmts() { return data; }

    clang::Expr* getRevSweepAsExpr() {
      return llvm::cast_or_null<clang::Expr>(getRevSweepStmt());
    }

    clang::Stmt* getRevSweepStmt() {
      /// If there is no specific value for
      /// the reverse sweep, use Stmt_dx.
      if (!m_ValueForRevSweep)
        return data[1];
      return m_ValueForRevSweep;
    }
  };

  template <typename T> class DeclDiff {
  private:
    std::array<T*, 2> m_data;

  public:
    DeclDiff(T* orig = nullptr, T* diff = nullptr) {
      m_data[1] = orig;
      m_data[0] = diff;
    }

    T* getDecl() { return m_data[1]; }
    T* getDecl_dx() { return m_data[0]; }
    // Decl_dx goes first!
    std::array<T*, 2>& getBothDecls() { return m_data; }
  };

  /// A base class for all common functionality for visitors
  class VisitorBase {
  protected:
    VisitorBase(DerivativeBuilder& builder, const DiffRequest& request)
        : m_Builder(builder), m_Sema(builder.m_Sema),
          m_CladPlugin(builder.m_CladPlugin), m_Context(builder.m_Context),
          m_DerivativeFnScope(nullptr), m_DerivativeInFlight(false),
          m_Derivative(nullptr), m_DiffReq(request) {}

    using Stmts = llvm::SmallVector<clang::Stmt*, 16>;

    DerivativeBuilder& m_Builder;
    clang::Sema& m_Sema;
    plugin::CladPlugin& m_CladPlugin;
    clang::ASTContext& m_Context;
    /// Current Scope at the point of visiting.
    /// Pointer to the topmost Scope in the created derivative function.
    clang::Scope* m_DerivativeFnScope;
    bool m_DerivativeInFlight;
    /// The Derivative function that is being generated.
    clang::FunctionDecl* m_Derivative;
    /// The differentiation request that is being currently processed.
    const DiffRequest& m_DiffReq;
    /// Map used to keep track of variable declarations and match them
    /// with their derivatives.
    std::unordered_map<const clang::ValueDecl*, clang::Expr*> m_Variables;
    /// Map contains variable declarations replacements. If the original
    /// function contains a declaration which name collides with something
    /// already created inside derivative's body, the declaration is replaced
    /// with a new one.
    /// See the example inside ForwardModeVisitor::VisitDeclStmt.
    std::unordered_map<const clang::VarDecl*, clang::VarDecl*>
        m_DeclReplacements;
    /// A stack of all the blocks where the statements of the gradient function
    /// are stored (e.g., function body, if statement blocks).
    std::vector<Stmts> m_Blocks;
    /// Stores derivative expression of the implicit `this` pointer.
    ///
    /// In the forward mode, `this` pointer derivative expression is of pointer
    /// type. In the reverse mode, `this` pointer derivative expression is of
    /// object type.
    // FIXME: Fix this inconsistency, by making `this` pointer derivative
    // expression to be of object type in the reverse mode as well.
    clang::Expr* m_ThisExprDerivative = nullptr;

    /// The currently visited statement. Useful for crash pretty-printing.
    const clang::Stmt* m_CurVisitedStmt = nullptr;

    /// A function used to wrap result of visiting E in a lambda. Returns a call
    /// to the built lambda. Func is a functor that will be invoked inside
    /// lambda scope and block. Statements inside lambda are expected to be
    /// added by addToCurrentBlock from func invocation.
    // FIXME: This will become problematic when we try to support C.
    template <typename F>
    static clang::Expr* wrapInLambda(VisitorBase& V, clang::Sema& S,
                                     const clang::Expr* E, F&& func) {
      // FIXME: Here we use some of the things that are used from Parser, it
      // seems to be the easiest way to create lambda.
      clang::LambdaIntroducer Intro;
      Intro.Default = clang::LCD_ByRef;
      // FIXME: Using noLoc here results in assert failure. Any other valid
      // SourceLocation seems to work fine.
      Intro.Range.setBegin(E->getBeginLoc());
      Intro.Range.setEnd(E->getEndLoc());
      clang::AttributeFactory AttrFactory;
      const clang::DeclSpec DS(AttrFactory);
      clang::Declarator D(DS,
                          CLAD_COMPAT_CLANG15_Declarator_DeclarationAttrs_ExtraParam
                          CLAD_COMPAT_CLANG12_Declarator_LambdaExpr);
#if CLANG_VERSION_MAJOR > 16
      V.beginScope(clang::Scope::LambdaScope | clang::Scope::DeclScope |

                   clang::Scope::FunctionDeclarationScope |
                   clang::Scope::FunctionPrototypeScope);
#endif // CLANG_VERSION_MAJOR
      S.PushLambdaScope();
#if CLANG_VERSION_MAJOR > 16
      S.ActOnLambdaExpressionAfterIntroducer(Intro, V.getCurrentScope());

      S.ActOnLambdaClosureParameters(V.getCurrentScope(), /*ParamInfo=*/{});
#endif // CLANG_VERSION_MAJOR

      V.beginScope(clang::Scope::BlockScope | clang::Scope::FnScope |
                   clang::Scope::DeclScope | clang::Scope::CompoundStmtScope);
      S.ActOnStartOfLambdaDefinition(Intro, D,
                   clad_compat::Sema_ActOnStartOfLambdaDefinition_ScopeOrDeclSpec(V.getCurrentScope(), DS));
#if CLANG_VERSION_MAJOR > 16
      V.endScope();
#endif // CLANG_VERSION_MAJOR

      V.beginBlock();
      func();
      clang::CompoundStmt* body = V.endBlock();
      clang::Expr* lambda =
          S.ActOnLambdaExpr(
               noLoc,
               body /*,*/
                   CLAD_COMPAT_CLANG17_ActOnLambdaExpr_getCurrentScope_ExtraParam(
                       V))
              .get();
      V.endScope();
      return S.ActOnCallExpr(V.getCurrentScope(), lambda, noLoc, {}, noLoc)
          .get();
    }
    /// For a qualtype QT returns if it's type is Array or Pointer Type
    static bool isArrayOrPointerType(const clang::QualType QT) {
      return utils::isArrayOrPointerType(QT);
    }

    clang::CompoundStmt* MakeCompoundStmt(const Stmts& Stmts);

    /// Get the latest block of code (i.e. place for statements output).
    Stmts& getCurrentBlock() { return m_Blocks.back(); }
    /// Create new block.
    Stmts& beginBlock() {
      m_Blocks.push_back({});
      return m_Blocks.back();
    }
    /// Remove the block from the stack, wrap it in CompoundStmt and return it.
    clang::CompoundStmt* endBlock() {
      auto CS = MakeCompoundStmt(getCurrentBlock());
      m_Blocks.pop_back();
      return CS;
    }

    // Check if result of the expression is unused.
    bool isUnusedResult(const clang::Expr* E);
    /// Output a statement to the current block. If Stmt is null or is an unused
    /// expression, it is not output and false is returned.
    bool addToCurrentBlock(clang::Stmt* S);
    bool addToBlock(clang::Stmt* S, Stmts& block);

    /// Get a current scope.
    /// FIXME: Remove the pointer-ref
    // clang::Scope* getCurrentScope() { return m_Sema.getCurScope(); }
    clang::Scope*& getCurrentScope();
    void setCurrentScope(clang::Scope* S);
    /// Returns the innermost enclosing file context which can be either a
    /// namespace or the TU scope.
    clang::Scope* getEnclosingNamespaceOrTUScope();

    /// Enters a new scope.
    void beginScope(unsigned ScopeFlags);
    void endScope();

    /// A shorthand to simplify syntax for creation of new expressions.
    /// This function uses m_Sema.BuildUnOp internally to build unary
    /// operations. Typical usage of this function looks like the following:
    /// \n \code
    /// auto postIncExpr = BuildOp(UO_PostInc, expr);
    /// auto assignExpr = BuildOp(BO_Assign, AsgnExpr, postIncExpr);
    /// addToCurrentBlock(assignExpr);
    /// \endcode
    /// \n The above will build the following expression:
    /// \n \code
    /// Asgn  = exp++;
    /// \endcode
    /// \param[in] OpCode The code for the unary operation to be built.
    /// \param[in] E The expression to build the unary operation with.
    /// \returns An expression of the newly built unary operation or null if the
    /// operand in null.
    clang::Expr* BuildOp(clang::UnaryOperatorKind OpCode, clang::Expr* E,
                         clang::SourceLocation OpLoc = noLoc);
    /// A shorthand to simplify syntax for creation of new expressions.
    /// This function uses m_Sema.BuildBin internally to build binary
    /// operations. A typical usage of this function looks like the following:
    /// \n \code
    /// auto mulExpr = BuildOp(BO_Mul, LExpr, RExpr);
    /// auto assignExpr = BuildOp(BO_Assign, AsgnExpr, mulExpr);
    /// addToCurrentBlock(assignExpr);
    /// \endcode
    /// \n The above will build the following expression:
    /// \n \code
    /// Asgn  = L * R;
    /// \endcode
    /// \param[in] OpCode The code for the binary operation to be built.
    /// \param[in] L The LHS expression to build the binary operation with.
    /// \param[in] R The RHS expression to build the binary operation with.
    /// \returns An expression of the newly built binary operation or null if
    /// either LHS or RHS is null.
    clang::Expr* BuildOp(clang::BinaryOperatorKind OpCode, clang::Expr* L,
                         clang::Expr* R, clang::SourceLocation OpLoc = noLoc);

    /// A shorthand to simplify syntax for creation of CXXOperatorCallExpr.
    /// We need it because Clang doesn't have a common ActOn- function to
    /// generate operator calls based on the operator kind. \param[in] OOK The
    /// kind of the operator. \param[in] ArgExprs The arguments of the operator.
    /// \param[in] OpLoc The source location, if necessary.
    /// \returns An expression of the built operator.
    clang::Expr* BuildOperatorCall(clang::OverloadedOperatorKind OOK,
                                   llvm::MutableArrayRef<clang::Expr*> ArgExprs,
                                   clang::SourceLocation OpLoc = noLoc);
    /// Function to resolve Unary Minus. If the leftmost operand
    /// has a Unary Minus then adds parens before adding the unary minus.
    /// \param[in] E Expression fed to the recursive call.
    /// \param[in] OpLoc Location to add Unary Minus if needed.
    /// \returns Expression with correct Unary Operator placement.
    clang::Expr* ResolveUnaryMinus(clang::Expr* E, clang::SourceLocation OpLoc);
    clang::Expr* BuildParens(clang::Expr* E);
    /// Sets Init as the initializer of the declaration VD and compute its
    /// initialization kind.
    ///\param[in] VD - variable declaration
    ///\param[in] Init - can be nullptr, then only initialization kind is
    /// computed.
    ///\param[in] DirectInit - tells whether the initialization is
    /// direct.
    void SetDeclInit(clang::VarDecl* VD, clang::Expr* Init = nullptr,
                     bool DirectInit = false);
    /// Builds variable declaration to be used inside the derivative
    /// body.
    /// \param[in] Type The type of variable declaration to build.
    /// \param[in] Identifier The identifier information for the variable
    /// declaration.
    /// \param[in] Init The initalization expression to assign to the variable
    ///  declaration.
    /// \param[in] DirectInit A check for if the initialization expression is a
    /// C style initalization.
    /// \param[in] TSI The type source information of the variable declaration.
    /// \returns The newly built variable declaration.
    clang::VarDecl*
    BuildVarDecl(clang::QualType Type, clang::IdentifierInfo* Identifier,
                 clang::Scope* scope, clang::Expr* Init = nullptr,
                 bool DirectInit = false, clang::TypeSourceInfo* TSI = nullptr);
    /// Builds variable declaration to be used inside the derivative
    /// body.
    /// \param[in] Type The type of variable declaration to build.
    /// \param[in] Identifier The identifier information for the variable
    /// declaration.
    /// \param[in] Init The initalization expression to assign to the variable
    ///  declaration.
    /// \param[in] DirectInit A check for if the initialization expression is a
    /// C style initalization.
    /// \param[in] TSI The type source information of the variable declaration.
    /// \returns The newly built variable declaration.
    clang::VarDecl* BuildVarDecl(clang::QualType Type,
                                 clang::IdentifierInfo* Identifier,
                                 clang::Expr* Init = nullptr,
                                 bool DirectInit = false,
                                 clang::TypeSourceInfo* TSI = nullptr);
    /// Builds variable declaration to be used inside the derivative
    /// body.
    /// \param[in] Type The type of variable declaration to build.
    /// \param[in] prefix The name of the variable declaration to build.
    /// \param[in] Init The initalization expression to assign to the variable
    ///  declaration.
    /// \param[in] DirectInit A check for if the initialization expression is a
    /// C style initalization.
    /// \param[in] TSI The type source information of the variable declaration.
    /// \returns The newly built variable declaration.
    clang::VarDecl* BuildVarDecl(clang::QualType Type,
                                 llvm::StringRef prefix = "_t",
                                 clang::Expr* Init = nullptr,
                                 bool DirectInit = false,
                                 clang::TypeSourceInfo* TSI = nullptr);
    /// Builds variable declaration to be used inside the derivative
    /// body in the derivative function global scope.
    clang::VarDecl* BuildGlobalVarDecl(clang::QualType Type,
                                       llvm::StringRef prefix = "_t",
                                       clang::Expr* Init = nullptr,
                                       bool DirectInit = false,
                                       clang::TypeSourceInfo* TSI = nullptr);
    /// Creates a namespace declaration and enters its context. All subsequent
    /// Stmts are built inside that namespace, until
    /// m_Sema.PopDeclContextIsUsed.
    clang::NamespaceDecl* BuildNamespaceDecl(clang::IdentifierInfo* II,
                                             bool isInline);
    /// Wraps a declaration in DeclStmt.
    /// \n Variable declaration cannot be added to code directly, instead we
    /// have to build a declaration staement.
    /// \param[in] D The declaration to build a declaration statement from.
    /// \returns The declaration statement expression corresponding to the input
    /// variable declaration.
    clang::DeclStmt* BuildDeclStmt(clang::Decl* D);
    /// Wraps a set of declarations in a DeclStmt.
    /// \n This function is useful to wrap multiple variable declarations in one
    /// single declaration statement.
    /// \param[in] D The declarations to build a declaration statement from.
    /// \returns The declaration statement expression corresponding to the input
    /// variable declaration.
    clang::DeclStmt* BuildDeclStmt(llvm::MutableArrayRef<clang::Decl*> DS);

    /// Builds a DeclRefExpr to a given Decl.
    /// \n To emit variables into code, we need to use their corresponding
    /// declaration reference expressions. This function builds a declaration
    /// reference given a declaration.
    /// \param[in] D The declaration to build a DeclRefExpr for.
    /// \param[in] SS The nested name specifier for the declaration.
    /// \returns the DeclRefExpr for the given declaration.
    clang::DeclRefExpr*
    BuildDeclRef(clang::DeclaratorDecl* D,
                 clang::NestedNameSpecifier* NNS = nullptr,
                 clang::ExprValueKind VK = clang::VK_LValue);

    /// Stores the result of an expression in a temporary variable (of the same
    /// type as is the result of the expression) and returns a reference to it.
    /// If force decl creation is true, this will allways create a temporary
    /// variable declaration. Otherwise, temporary variable is created only
    /// if E requires evaluation (e.g. there is no point to store literals or
    /// direct references in intermediate variables)
    clang::Expr* StoreAndRef(clang::Expr* E, Stmts& block,
                             llvm::StringRef prefix = "_t",
                             bool forceDeclCreation = false);
    /// A shorthand to store directly to the current block.
    clang::Expr* StoreAndRef(clang::Expr* E, llvm::StringRef prefix = "_t",
                             bool forceDeclCreation = false);
    /// An overload allowing to specify the type for the variable.
    clang::Expr* StoreAndRef(clang::Expr* E, clang::QualType Type, Stmts& block,
                             llvm::StringRef prefix = "_t",
                             bool forceDeclCreation = false);
    /// For an expr E, decides if it is useful to store it in a temporary
    /// variable and replace E's further usage by a reference to that variable
    /// to avoid recomputation.
    static bool UsefulToStore(clang::Expr* E);
    /// A flag for silencing warnings/errors output by diag function.
    /// Shorthand to issues a warning or error.
    template <std::size_t N>
    void diag(clang::DiagnosticsEngine::Level level, // Warning or Error
              clang::SourceLocation loc, const char (&format)[N],
              llvm::ArrayRef<llvm::StringRef> args = {}) {
      m_Builder.diag(level, loc, format, args);
    }

    /// Creates unique identifier of the form "_nameBase<number>" that is
    /// guaranteed not to collide with anything in the current scope.
    clang::IdentifierInfo* CreateUniqueIdentifier(llvm::StringRef nameBase);
    std::unordered_map<std::string, std::size_t> m_idCtr;

    /// Updates references in newly cloned statements.
    void updateReferencesOf(clang::Stmt* InSubtree);

    /// Get an expression used to zero-initialize given type.
    /// Returns 0 for scalar types, otherwise {}.
    clang::Expr* getZeroInit(clang::QualType T);

    /// Split an array subscript expression into a pair of base expr and
    /// a vector of all indices.
    std::pair<const clang::Expr*, llvm::SmallVector<const clang::Expr*, 4>>
    SplitArraySubscript(const clang::Expr* ASE);

    /// Build an array subscript expression with a given base expression and
    /// a sequence of indices.
    clang::Expr*
    BuildArraySubscript(clang::Expr* Base,
                        const llvm::SmallVectorImpl<clang::Expr*>& IS);

    /// Build an array subscript expression with a given base expression and
    /// one index.
    clang::Expr* BuildArraySubscript(clang::Expr* Base, clang::Expr*& Idx) {
      llvm::SmallVector<clang::Expr*, 1> IS = {Idx};
      return BuildArraySubscript(Base, IS);
    }
    /// Find declaration of clad::tape templated type.
    clang::TemplateDecl* GetCladTapeDecl();
    /// Perform a lookup into clad namespace for an entity with given name.
    clang::LookupResult LookupCladTapeMethod(llvm::StringRef name);
    /// Perform lookup into clad namespace for push/pop/back. Returns
    /// LookupResult, which is will be resolved later (which is handy since they
    /// are templates).
    clang::LookupResult& GetCladTapePush();
    clang::LookupResult& GetCladTapePop();
    clang::LookupResult& GetCladTapeBack();
    /// Instantiate clad::tape<T> type.
    clang::QualType GetCladTapeOfType(clang::QualType T);

    /// Helper to build a function call expression.
    ///
    /// \param[in] funcName The name of the function to build the expression
    /// for.
    /// \param[in] nmspace The name of the namespace for the function,
    /// currently does not support nested namespaces.
    /// \param[in] callArgs A vector of \c clang::Expr of all the parameters
    /// to the function call.
    ///
    /// \return The function call expression that can be used to emit into
    /// code.
    clang::Expr* GetFunctionCall(const std::string& funcName,
                                 const std::string& nmspace,
                                 llvm::SmallVectorImpl<clang::Expr*>& callArgs);

    clang::DeclRefExpr* GetCladTapePushDRE();

    clang::Stmt* GetCladZeroInit(llvm::MutableArrayRef<clang::Expr*> args);

    /// Assigns the Init expression to VD after performing the necessary
    /// implicit conversion. This is required as clang doesn't add implicit
    /// conversions while assigning values to variables which are initialized
    /// after it is already declared.
    void PerformImplicitConversionAndAssign(clang::VarDecl* VD,
                                            clang::Expr* Init) {
      // Implicitly convert Init into the type of VD
      clang::ActionResult<clang::Expr*> ICAR = m_Sema.PerformImplicitConversion(
          Init, VD->getType(), CLAD_COMPAT_CLANG20_SemaAACasting);
      assert(!ICAR.isInvalid() && "Invalid implicit conversion!");
      // Assign the resulting expression to the variable declaration
      SetDeclInit(VD, ICAR.get());
    }

    /// Build a call to member function through Base expr and using the function
    /// name.
    ///
    /// \param[in] Base expr to the object which is used to call the member
    ///  function
    /// \param[in] isArrow if true specifies that the member function is
    /// accessed by an -> otherwise .
    /// \param[in] MemberFunctionName the name of the member function
    /// \param[in] ArgExprs the arguments to be used when calling the member
    ///  function
    /// \returns Built member function call expression
    ///  Base.MemberFunction(ArgExprs) or Base->MemberFunction(ArgExprs)
    clang::Expr*
    BuildCallExprToMemFn(clang::Expr* Base, llvm::StringRef MemberFunctionName,
                         llvm::MutableArrayRef<clang::Expr*> ArgExprs,
                         clang::SourceLocation Loc = noLoc);

    /// Build a call to member function through this pointer.
    ///
    /// \param[in] FD callee member function
    /// \param[in] argExprs function arguments expressions
    /// \param[in] useRefQualifiedThisObj If true, then the `this` object is
    /// perfectly forwarded while calling member functions.
    /// \returns Built member function call expression
    clang::Expr* BuildCallExprToMemFn(
        clang::CXXMethodDecl* FD, llvm::MutableArrayRef<clang::Expr*> argExprs,
        bool useRefQualifiedThisObj = false, clang::SourceLocation Loc = noLoc);

    /// Build a call to a free function or member function through
    /// this pointer depending on whether the `FD` argument corresponds to a
    /// free function or a member function.
    ///
    /// \param[in] FD callee function
    /// \param[in] argExprs function arguments expressions
    /// \param[in] useRefQualifiedThisObj If true, then the `this` object is
    /// perfectly forwarded while calling member functions.
    /// \returns Built call expression
    clang::Expr*
    BuildCallExprToFunction(clang::FunctionDecl* FD,
                            llvm::MutableArrayRef<clang::Expr*> argExprs,
                            bool useRefQualifiedThisObj = false);

    /// Build a call to templated free function inside the clad namespace.
    ///
    /// \param[in] name name of the function
    /// \param[in] argExprs function arguments expressions
    /// \param[in] templateArgs template arguments
    /// \param[in] loc location of the call
    /// \returns Built call expression
    clang::Expr* BuildCallExprToCladFunction(
        llvm::StringRef name, llvm::MutableArrayRef<clang::Expr*> argExprs,
        llvm::ArrayRef<clang::TemplateArgument> templateArgs,
        clang::SourceLocation loc);

    /// Checks if the type is of clad::array<T> or clad::array_ref<T> type
    bool isCladArrayType(clang::QualType QT);
    /// Creates the expression clad::matrix<T>::identity(Args) for the given
    /// type and args.
    clang::Expr*
    BuildIdentityMatrixExpr(clang::QualType T,
                            llvm::MutableArrayRef<clang::Expr*> Args,
                            clang::SourceLocation Loc);
    /// Creates the expression Base.size() for the given Base expr. The Base
    /// expr must be of clad::array_ref<T> type
    clang::Expr* BuildArrayRefSizeExpr(clang::Expr* Base);
    /// Creates the expression Base.slice(Args) for the given Base expr and Args
    /// array. The Base expr must be of clad::array_ref<T> type
    clang::Expr*
    BuildArrayRefSliceExpr(clang::Expr* Base,
                           llvm::MutableArrayRef<clang::Expr*> Args);
    clang::ParmVarDecl* CloneParmVarDecl(const clang::ParmVarDecl* PVD,
                                         clang::IdentifierInfo* II,
                                         bool pushOnScopeChains = false,
                                         bool cloneDefaultArg = true,
                                         clang::SourceLocation Loc = noLoc);
    /// A function to get the single argument "forward_central_difference"
    /// call expression for the given arguments.
    ///
    /// \param[in] targetFuncCall The function to get the derivative for.
    /// \param[in] targetArg The argument to get the derivative with respect to.
    /// \param[in] targetPos The relative position of 'targetArg'.
    /// \param[in] numArgs The total number of 'args'.
    /// \param[in] args All the arguments to the target function.
    ///
    /// \returns The derivative function call.
    clang::Expr* GetSingleArgCentralDiffCall(
        clang::Expr* targetFuncCall, clang::Expr* targetArg, unsigned targetPos,
        unsigned numArgs, llvm::SmallVectorImpl<clang::Expr*>& args,
        clang::Expr* CUDAExecConfig = nullptr);

    /// Emits diagnostic messages on differentiation (or lack thereof) for
    /// call expressions.
    ///
    /// \param[in] \c FD - The function declaration.
    /// \param[in] \c srcLoc Any associated source location information.
    void CallExprDiffDiagnostics(const clang::FunctionDecl* FD,
                                 clang::SourceLocation srcLoc);

    clang::QualType DetermineCladArrayValueType(clang::QualType T);

    /// Returns clad::Identify template declaration.
    clang::TemplateDecl* GetCladConstructorPushforwardTag();

    /// Returns type clad::Identify<T>
    clang::QualType GetCladConstructorPushforwardTagOfType(clang::QualType T);

    /// Returns clad::ConstructorReverseForwTag template declaration.
    clang::TemplateDecl* GetCladConstructorReverseForwTag();

    /// Returns type clad::ConstructorReverseForwTag<T>
    clang::QualType GetCladConstructorReverseForwTagOfType(clang::QualType T);

    /// Builds an overload for the derivative function that has derived params
    /// for all the arguments of the requested function and it calls the
    /// original derivative function internally. Used in gradient and jacobian
    /// modes.
    clang::FunctionDecl* CreateDerivativeOverload();
    /// Find the derived function if present in the DerivedFnCollector.
    ///
    /// \param[in] request The request to find the derived function.
    ///
    /// \returns The derived function if found, nullptr otherwise.
    clang::FunctionDecl* FindDerivedFunction(DiffRequest& request);

  public:
    /// Rebuild a sequence of nested namespaces ending with DC.
    clang::NamespaceDecl* RebuildEnclosingNamespaces(clang::DeclContext* DC);
    /// Clones a statement
    clang::Stmt* Clone(const clang::Stmt* S);
    /// A shorthand to simplify cloning of expressions.
    clang::Expr* Clone(const clang::Expr* E);
    /// Cloning types is necessary since VariableArrayType
    /// store a pointer to their size expression.
    clang::QualType CloneType(clang::QualType T);

    /// Initiates the differentiation process.
    /// Returns the derivative and its overload, if any.
    virtual DerivativeAndOverload Derive() = 0;

    /// Builds the QualType of the derivative to be generated.
    ///
    /// \param[in] moveBaseToParams If true, turns member functions into regular
    /// functions by moving the base to the parameters.
    clang::QualType
    GetDerivativeType(llvm::ArrayRef<clang::QualType> customParams = {});

    /// Computes effective derivative operands. It should be used when operands
    /// might be of pointer types.
    ///
    /// In the trivial case, both operands are of non-pointer types, and the
    /// effective derivative operands are `LDiff.getExpr_dx()` and
    /// `RDiff.getExpr_dx()` respectively.
    ///
    /// Integers used in pointer arithmetic should be considered
    /// non-differentiable entities. For example:
    ///
    /// ```
    /// p + i;
    /// ```
    ///
    /// Derived statement should be:
    ///
    /// ```
    /// _d_p + i;
    /// ```
    ///
    /// instead of:
    ///
    /// ```
    /// _d_p + _d_i;
    /// ```
    ///
    /// Therefore, effective derived expression of `i` is `i` instead of `_d_i`.
    ///
    /// This functions sets `derivedL` and `derivedR` arguments to effective
    /// derived expressions.
    void ComputeEffectiveDOperands(StmtDiff& LDiff, StmtDiff& RDiff,
                                   clang::Expr*& derivedL,
                                   clang::Expr*& derivedR);

    virtual ~VisitorBase() = 0;

  private:
    clang::TemplateDecl* m_CladConstructorPushforwardTag = nullptr;
    clang::TemplateDecl* m_CladConstructorReverseForwTag = nullptr;
  };

  /// A class that generates prettier stack traces when we crash on generating
  /// a derivative.
  class PrettyStackTraceDerivative : public llvm::PrettyStackTraceEntry {
    const DiffRequest& m_DiffReq;
    using Blocks = std::vector<llvm::SmallVector<clang::Stmt*, 16>>;
    const Blocks& m_Blocks;
    const clang::Sema& m_Sema;
    const clang::Stmt** m_Stmt = nullptr;

  public:
    PrettyStackTraceDerivative(const DiffRequest& DiffReq, const Blocks& B,
                               const clang::Sema& Sema, const clang::Stmt** S)
        : m_DiffReq(DiffReq), m_Blocks(B), m_Sema(Sema), m_Stmt(S) {}
    void print(llvm::raw_ostream& OS) const override;
  };

} // end namespace clad

#endif // CLAD_VISITOR_BASE_H
