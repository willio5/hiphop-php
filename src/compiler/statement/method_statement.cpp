/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010 Facebook, Inc. (http://www.facebook.com)          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <compiler/statement/method_statement.h>
#include <compiler/statement/return_statement.h>
#include <compiler/analysis/analysis_result.h>
#include <compiler/expression/modifier_expression.h>
#include <compiler/expression/expression_list.h>
#include <compiler/expression/constant_expression.h>
#include <compiler/expression/parameter_expression.h>
#include <compiler/analysis/code_error.h>
#include <compiler/analysis/file_scope.h>
#include <compiler/analysis/variable_table.h>
#include <compiler/analysis/class_scope.h>
#include <compiler/analysis/function_scope.h>
#include <compiler/statement/statement_list.h>
#include <util/util.h>
#include <compiler/option.h>
#include <compiler/analysis/dependency_graph.h>
#include <compiler/builtin_symbols.h>
#include <compiler/analysis/alias_manager.h>

using namespace HPHP;
using namespace std;
using namespace boost;

///////////////////////////////////////////////////////////////////////////////
// constructors/destructors

MethodStatement::MethodStatement
(STATEMENT_CONSTRUCTOR_PARAMETERS,
 ModifierExpressionPtr modifiers, bool ref, const std::string &name,
 ExpressionListPtr params, StatementListPtr stmt, int attr,
 const std::string &docComment, bool method /* = true */)
  : Statement(STATEMENT_CONSTRUCTOR_PARAMETER_VALUES),
    m_method(method), m_modifiers(modifiers), m_ref(ref), m_originalName(name),
    m_params(params), m_stmt(stmt), m_attribute(attr),
    m_docComment(docComment) {
  m_name = Util::toLower(name);
}

StatementPtr MethodStatement::clone() {
  MethodStatementPtr stmt(new MethodStatement(*this));
  stmt->m_stmt = Clone(m_stmt);
  stmt->m_params = Clone(m_params);
  stmt->m_modifiers = Clone(m_modifiers);
  return stmt;
}

BlockScopePtr MethodStatement::getScope() {
  return m_funcScope.lock();
}

std::string MethodStatement::getFullName() const {
  if (m_className.empty()) return m_name;
  return m_className + "::" + m_name;
}

bool MethodStatement::isRef(int index /* = -1 */) const {
  if (index == -1) return m_ref;
  ASSERT(index >= 0 && index < m_params->getCount());
  ParameterExpressionPtr param =
    dynamic_pointer_cast<ParameterExpression>((*m_params)[index]);
  return param->isRef();
}

int MethodStatement::getRecursiveCount() const {
  return m_stmt ? m_stmt->getRecursiveCount() : 0;
}

///////////////////////////////////////////////////////////////////////////////
// parser functions

FunctionScopePtr MethodStatement::onParseImpl(AnalysisResultPtr ar) {
  int minParam, maxParam;
  ConstructPtr self = shared_from_this();
  minParam = maxParam = 0;
  bool hasRef = false;
  if (m_params) {
    set<string> names;
    int i = 0;
    maxParam = m_params->getCount();
    for (; i < maxParam; i++) {
      ParameterExpressionPtr param =
        dynamic_pointer_cast<ParameterExpression>((*m_params)[i]);
      if (param->isRef()) hasRef = true;
      if (param->isOptional()) break;
      minParam++;
    }
    for (i++; i < maxParam; i++) {
      ParameterExpressionPtr param =
        dynamic_pointer_cast<ParameterExpression>((*m_params)[i]);
      if (param->isRef()) hasRef = true;
      if (!param->isOptional()) {
        ar->getCodeError()->record(self, CodeError::RequiredAfterOptionalParam,
                                   param);
        param->defaultToNull(ar);
      }
    }

    if (ar->isFirstPass()) {
      for (i = 0; i < maxParam; i++) {
        ParameterExpressionPtr param =
          dynamic_pointer_cast<ParameterExpression>((*m_params)[i]);
        if (names.find(param->getName()) == names.end()) {
          names.insert(param->getName());
        } else {
          ar->getCodeError()->record(self,
                                     CodeError::RedundantParameter, param);
          for (int j = 0; j < 1000; j++) {
            string name = param->getName() + lexical_cast<string>(j);
            if (names.find(name) == names.end()) {
              param->rename(name);
              break;
            }
          }
        }
      }
    }
  }

  if (hasRef || m_ref) {
    m_attribute |= FileScope::ContainsReference;
  }

  StatementPtr stmt = dynamic_pointer_cast<Statement>(shared_from_this());
  FunctionScopePtr funcScope
    (new FunctionScope(ar, m_method, m_name, stmt, m_ref, minParam, maxParam,
                       m_modifiers, m_attribute, m_docComment,
                       ar->getFileScope()));
  if (!m_stmt) {
    funcScope->setVirtual();
  }
  m_funcScope = funcScope;

  // TODO: this may have to expand to a concept of "virtual" functions...
  if (ar->getClassScope()) {
    funcScope->disableInline();
    if (m_name.length() > 2 && m_name.substr(0,2) == "__") {
      bool magic = true;
      int paramCount = 0;
      if (m_name == "__destruct") {
        funcScope->setOverriding(Type::Variant);
      } else if (m_name == "__call") {
        funcScope->setOverriding(Type::Variant, Type::String, Type::Array);
        paramCount = 2;
      } else if (m_name == "__set") {
        funcScope->setOverriding(Type::Variant, Type::String, Type::Variant);
        paramCount = 2;
      } else if (m_name == "__get") {
        funcScope->setOverriding(Type::Variant, Type::String);
        paramCount = 1;
      } else if (m_name == "__isset") {
        funcScope->setOverriding(Type::Boolean, Type::String);
        paramCount = 1;
      } else if (m_name == "__unset") {
        funcScope->setOverriding(Type::Variant, Type::String);
        paramCount = 1;
      } else if (m_name == "__sleep") {
        funcScope->setOverriding(Type::Variant);
      } else if (m_name == "__wakeup") {
        funcScope->setOverriding(Type::Variant);
      } else if (m_name == "__set_state") {
        funcScope->setOverriding(Type::Variant, Type::Variant);
        paramCount = 1;
      } else if (m_name == "__tostring") {
        funcScope->setOverriding(Type::String);
      } else if (m_name == "__clone") {
        funcScope->setOverriding(Type::Variant);
      } else {
        paramCount = -1;
        if (m_name != "__construct") {
          ar->getCodeError()->record(self, CodeError::UnknownMagicMethod,
                                     self);
          magic = false;
        }
      }
      if (paramCount >= 0 && paramCount != maxParam) {
        ar->getCodeError()->record(self, CodeError::InvalidMagicMethod,
                                   self);
        magic = false;
      }
      if (magic) funcScope->setMagicMethod();
    }
    // ArrayAccess methods
    else if (m_name.length() > 6 && m_name.substr(0, 6) == "offset") {
      if (m_name == "offsetexists") {
        funcScope->setOverriding(Type::Boolean, Type::Variant);
      } else if (m_name == "offsetget") {
        funcScope->setOverriding(Type::Variant, Type::Variant);
      } else if (m_name == "offsetset") {
        funcScope->setOverriding(Type::Variant, Type::Variant, Type::Variant);
      } else if (m_name == "offsetunset") {
        funcScope->setOverriding(Type::Variant, Type::Variant);
      }
    }
  }

  return funcScope;
}

void MethodStatement::onParse(AnalysisResultPtr ar) {
  ClassScopePtr classScope =
    dynamic_pointer_cast<ClassScope>(ar->getScope());

  classScope->addFunction(ar, onParseImpl(ar));
  if (m_name == "__construct") {
    classScope->setAttribute(ClassScope::HasConstructor);
  } else if (m_name == "__destruct") {
    classScope->setAttribute(ClassScope::HasDestructor);
  }

  if (m_name == "__call") {
    classScope->setAttribute(ClassScope::HasUnknownMethodHandler);
  } else if (m_name == "__get") {
    classScope->setAttribute(ClassScope::HasUnknownPropHandler);
  }

  m_className = classScope->getName();
}

///////////////////////////////////////////////////////////////////////////////
// static analysis functions

void MethodStatement::addParamRTTI(AnalysisResultPtr ar) {
  FunctionScopePtr func =
    dynamic_pointer_cast<FunctionScope>(ar->getScope());
  VariableTablePtr variables = func->getVariables();
  if (variables->getAttribute(VariableTable::ContainsDynamicVariable) ||
      variables->getAttribute(VariableTable::ContainsExtract)) {
    return;
  }
  for (int i = 0; i < m_params->getCount(); i++) {
    ParameterExpressionPtr param =
      dynamic_pointer_cast<ParameterExpression>((*m_params)[i]);
    const string &paramName = param->getName();
    if (variables->isLvalParam(paramName)) continue;
    TypePtr paramType = param->getActualType();
    if ((paramType->is(Type::KindOfVariant) ||
         paramType->is(Type::KindOfSome)) &&
        !param->isRef()) {
      param->setHasRTTI();
      ClassScopePtr cls = ar->getClassScope();
      ar->addParamRTTIEntry(cls, func, paramName);
      const string funcId = ar->getFuncId(cls, func);
      ar->addRTTIFunction(funcId);
    }
  }
}

void MethodStatement::analyzeProgramImpl(AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();

  // registering myself as a parent in dependency graph, so that
  // (1) we can tell orphaned parents
  // (2) overwrite non-master copy of function declarations
  if (ar->isFirstPass()) {
    ar->getDependencyGraph()->addParent(DependencyGraph::KindOfFunctionCall,
                                        "", getFullName(), shared_from_this());
    if (Option::AllDynamic || hasHphpNote("Dynamic") ||
        funcScope->isSepExtension() ||
        BuiltinSymbols::IsDeclaredDynamic(m_name) ||
        Option::IsDynamicFunction(m_method, m_name)) {
      funcScope->setDynamic();
    }
    if (hasHphpNote("Volatile")) funcScope->setVolatile();
  }

  funcScope->setIncludeLevel(ar->getIncludeLevel());
  ar->pushScope(funcScope);
  if (m_params) {
    m_params->analyzeProgram(ar);
    if (Option::GenRTTIProfileData &&
        ar->getPhase() == AnalysisResult::AnalyzeFinal) {
      addParamRTTI(ar);
    }
  }
  if (m_stmt) m_stmt->analyzeProgram(ar);

  if (ar->isFirstPass()) {
    if (!funcScope->isStatic() && ar->getClassScope() &&
        funcScope->getVariables()->
        getAttribute(VariableTable::ContainsDynamicVariable)) {
      // Add this to variable table if we'll need it in a lookup table
      // Use object because there's no point to specializing, just makes
      // code gen harder when dealing with redeclared classes.
      TypePtr tp(NEW_TYPE(Object));
      funcScope->getVariables()->add("this", tp, true, ar, shared_from_this(),
                                     ModifierExpressionPtr());
    }
    FunctionScope::RecordRefParamInfo(m_name, funcScope);
  }
  ar->popScope();
}

ConstructPtr MethodStatement::getNthKid(int n) const {
  switch (n) {
    case 0:
      return m_modifiers;
    case 1:
      return m_params;
    case 2:
      return m_stmt;
    default:
      ASSERT(false);
      break;
  }
  return ConstructPtr();
}

int MethodStatement::getKidCount() const {
  return 3;
}

void MethodStatement::setNthKid(int n, ConstructPtr cp) {
  switch (n) {
    case 0:
      m_modifiers = boost::dynamic_pointer_cast<ModifierExpression>(cp);
      break;
    case 1:
      m_params = boost::dynamic_pointer_cast<ExpressionList>(cp);
      break;
    case 2:
      m_stmt = boost::dynamic_pointer_cast<StatementList>(cp);
      break;
    default:
      ASSERT(false);
      break;
  }
}

StatementPtr MethodStatement::preOptimize(AnalysisResultPtr ar) {
  ar->preOptimize(m_modifiers);
  ar->preOptimize(m_params);
  FunctionScopePtr funcScope = m_funcScope.lock();
  ar->pushScope(funcScope);
  if (ar->getPhase() != AnalysisResult::AnalyzeInclude &&
      Option::LocalCopyProp) {
    int flag;
    do {
      AliasManager am;
      MethodStatementPtr self =
        static_pointer_cast<MethodStatement>(shared_from_this());
      flag = am.optimize(ar, self);
      if (flag >= 0) {
        ar->preOptimize(m_stmt);
      }
    } while (flag);
  } else {
    ar->preOptimize(m_stmt);
  }
  ar->popScope();
  return StatementPtr();
}

StatementPtr MethodStatement::postOptimize(AnalysisResultPtr ar) {
  ar->postOptimize(m_modifiers);
  ar->postOptimize(m_params);
  FunctionScopePtr funcScope = m_funcScope.lock();
  ar->pushScope(funcScope);
  if (ar->getPhase() != AnalysisResult::AnalyzeInclude &&
      (Option::LocalCopyProp || Option::StringLoopOpts)) {
    int flag;
    do {
      AliasManager am;
      MethodStatementPtr self =
        static_pointer_cast<MethodStatement>(shared_from_this());
      flag = am.optimize(ar, self);
      if (flag >= 0) {
        ar->postOptimize(m_stmt);
      }
    } while (flag);
  } else {
    ar->postOptimize(m_stmt);
  }
  ar->popScope();
  return StatementPtr();
}
void MethodStatement::inferTypes(AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  if (ar->getPhase() == AnalysisResult::FirstInference && m_stmt) {
    if (m_stmt->hasRetExp() ||
        funcScope->inPseudoMain() ||
        funcScope->getReturnType()) {
      bool lastIsReturn = false;
      if (m_stmt->getCount()) {
        StatementPtr lastStmt = (*m_stmt)[m_stmt->getCount()-1];
        if (lastStmt->is(Statement::KindOfReturnStatement)) {
          lastIsReturn = true;
        }
      }
      if (!lastIsReturn &&
          !(funcScope->inPseudoMain() && !Option::GenerateCPPMain)) {
        ExpressionPtr constant =
          funcScope->inPseudoMain() ? CONSTANT("true") : CONSTANT("null");
        ReturnStatementPtr returnStmt =
          ReturnStatementPtr(new ReturnStatement(getLocation(),
            Statement::KindOfReturnStatement, constant));
        m_stmt->addElement(returnStmt);
      }
    }
  }

  ar->pushScope(funcScope);
  if (m_params) {
    m_params->inferAndCheck(ar, NEW_TYPE(Any), false);
  }
  if (m_stmt) {
    m_stmt->inferTypes(ar);
  }
  ar->popScope();
}

///////////////////////////////////////////////////////////////////////////////
// code generation functions

void MethodStatement::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  if (ar) ar->pushScope(funcScope);

  m_modifiers->outputPHP(cg, ar);
  cg_printf(" function ");
  if (m_ref) cg_printf("&");
  cg_printf("%s(", m_originalName.c_str());
  if (m_params) m_params->outputPHP(cg, ar);
  if (m_stmt) {
    cg_indentBegin(") {\n");
    funcScope->outputPHP(cg, ar);
    m_stmt->outputPHP(cg, ar);
    cg_indentEnd("}\n");
  } else {
    cg_printf(");\n");
  }

  if (ar) ar->popScope();
}

void MethodStatement::outputCPPImpl(CodeGenerator &cg, AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  ClassScopePtr scope = ar->getClassScope();
  ar->pushScope(funcScope);

  if (outputFFI(cg, ar)) return;

  cg.setPHPLineNo(-1);

  if (cg.getContext() == CodeGenerator::CppImplementation) {
    printSource(cg);
  }

  switch (cg.getContext()) {
  case CodeGenerator::CppDeclaration:
    {
      if (!m_stmt && !funcScope->isPerfectVirtual()) {
        cg_printf("// ");
      }

      m_modifiers->outputCPP(cg, ar);

      if (!m_stmt || m_name == "__offsetget_lval" ||
          funcScope->isPerfectVirtual()) {
        cg_printf("virtual ");
      }
      TypePtr type = funcScope->getReturnType();
      if (type) {
        type->outputCPPDecl(cg, ar);
      } else {
        cg_printf("void");
      }
      if (m_name == "__lval") {
        cg_printf(" &___lval(");
      } else if (m_name == "__offsetget_lval") {
        cg_printf(" &___offsetget_lval(");
      } else if (m_modifiers->isStatic() && m_stmt) {
        // Static method wrappers get generated as support methods
        cg_printf(" %s%s(const char* cls%s", Option::MethodImplPrefix,
                  cg.formatLabel(m_name).c_str(),
                  funcScope->isVariableArgument() ||
                  (m_params && m_params->getCount()) ? ", " : "");
      } else {
        cg_printf(" %s%s(", Option::MethodPrefix,
                  cg.formatLabel(m_name).c_str());
      }
      funcScope->outputCPPParamsDecl(cg, ar, m_params, true);
      if (m_stmt) {
        cg_printf(");\n");
      } else if (funcScope->isPerfectVirtual()) {
        cg_printf(") { return throw_fatal(\"pure virtual\");}\n");
      } else {
        cg_printf(") = 0;\n");
      }

      if (funcScope->isConstructor(scope)
       && !funcScope->isAbstract() && !scope->isInterface()) {
        funcScope->outputCPPCreateDecl(cg, ar);
      }
    }
    break;
  case CodeGenerator::CppImplementation:
    if (m_stmt) {
      TypePtr type = funcScope->getReturnType();
      if (type) {
        type->outputCPPDecl(cg, ar);
      } else {
        cg_printf("void");
      }
      string origFuncName = std::string(scope->getOriginalName()) +
                            "::" + m_originalName;
      string funcSection = Option::FunctionSections[origFuncName];
      if (!funcSection.empty()) {
        cg_printf(" __attribute__ ((section (\".text.%s\")))",
                  funcSection.c_str());
      }

      if (m_name == "__lval") {
        cg_printf(" &%s%s::___lval(",
                  Option::ClassPrefix, scope->getId(cg).c_str());
      } else if (m_name == "__offsetget_lval") {
        cg_printf(" &%s%s::___offsetget_lval(",
                  Option::ClassPrefix, scope->getId(cg).c_str());
      } else if (m_modifiers->isStatic()) {
        cg_printf(" %s%s::%s%s(const char* cls%s", Option::ClassPrefix,
                  scope->getId(cg).c_str(),
                  Option::MethodImplPrefix, cg.formatLabel(m_name).c_str(),
                  funcScope->isVariableArgument() ||
                  (m_params && m_params->getCount()) ? ", " : "");
      } else {
        cg_printf(" %s%s::%s%s(", Option::ClassPrefix,
                  scope->getId(cg).c_str(),
                  Option::MethodPrefix, cg.formatLabel(m_name).c_str());
      }
      funcScope->outputCPPParamsDecl(cg, ar, m_params, false);
      cg_indentBegin(") {\n");
      if (m_stmt->hasBody()) {
        const char *sys =
          (cg.getOutput() == CodeGenerator::SystemCPP ? "_BUILTIN" : "");
        if (m_modifiers->isStatic()) {
          cg_printf("STATIC_METHOD_INJECTION%s(%s, %s);\n", sys,
                    scope->getOriginalName().c_str(), origFuncName.c_str());
        } else {
          cg_printf("INSTANCE_METHOD_INJECTION%s(%s, %s);\n", sys,
                    scope->getOriginalName().c_str(), origFuncName.c_str());
        }
      }
      if (Option::GenRTTIProfileData && m_params) {
        for (int i = 0; i < m_params->getCount(); i++) {
          ParameterExpressionPtr param =
            dynamic_pointer_cast<ParameterExpression>((*m_params)[i]);
          if (param->hasRTTI()) {
            const string &paramName = param->getName();
            int id = ar->getParamRTTIEntryId(ar->getClassScope(), funcScope,
                                             paramName);
            if (id != -1) {
              cg_printf("RTTI_INJECTION(%s%s, %d);\n",
                        Option::VariablePrefix, paramName.c_str(), id);
            }
          }
        }
      }
      if (m_name == "__lval" || m_name == "__offsetget_lval") {
        ParameterExpressionPtr param =
          dynamic_pointer_cast<ParameterExpression>((*m_params)[0]);
        cg_printf("Variant &v = %s->__lvalProxy;\n",
                  cg.getOutput() == CodeGenerator::SystemCPP ?
                  "get_system_globals()" : "get_global_variables()");
        string lowered = Util::toLower(m_originalName);
        cg_printf("v = %s%s(%s%s);\n",
                  Option::MethodPrefix, lowered.c_str(),
                  Option::VariablePrefix, param->getName().c_str());
        cg_printf("return v;\n");
      } else {
        if (funcScope->isConstructor(scope)) {
          cg_printf("bool oldInCtor = gasInCtor(true);\n");
        } else if (m_name == "__destruct") {
          cg_printf("setInDtor();\n");
        }
        funcScope->outputCPP(cg, ar);
        cg.setContext(CodeGenerator::NoContext); // no inner functions/classes
        if (!funcScope->isStatic() && funcScope->getVariables()->
            getAttribute(VariableTable::ContainsDynamicVariable)) {
          cg_printf("%sthis = this;\n", Option::VariablePrefix);
        }
        outputCPPStmt(cg, ar);
        cg.setContext(CodeGenerator::CppImplementation);
      }
      cg_indentEnd("} /* function */\n");
    }
    break;
  default:
    break;
  }

  ar->popScope();
}

void MethodStatement::outputCPPStmt(CodeGenerator &cg, AnalysisResultPtr ar) {
  if (m_stmt) {
    m_stmt->outputCPP(cg, ar);
    if (!m_stmt->hasRetExp()) {
      FunctionScopePtr funcScope = m_funcScope.lock();
      ClassScopePtr cls = ar->getClassScope();
      if (funcScope->isConstructor(cls)) {
        cg_printf("gasInCtor(oldInCtor);\n");
      }
    }
  }
}

void MethodStatement::outputCPPStaticMethodWrapper(CodeGenerator &cg,
                                                   AnalysisResultPtr ar,
                                                   const char *cls) {
  if (!m_modifiers->isStatic() || !m_stmt) return;
  FunctionScopePtr funcScope = m_funcScope.lock();
  ar->pushScope(funcScope);
  m_modifiers->outputCPP(cg, ar);
  TypePtr type = funcScope->getReturnType();
  if (type) {
    type->outputCPPDecl(cg, ar);
  } else {
    cg_printf("void");
  }
  cg_printf(" %s%s(", Option::MethodPrefix, cg.formatLabel(m_name).c_str());
  CodeGenerator::Context old = cg.getContext();
  cg.setContext(CodeGenerator::CppStaticMethodWrapper);
  funcScope->outputCPPParamsDecl(cg, ar, m_params, true);
  cg.setContext(old);
  cg_printf(") { %s%s%s(\"%s\"", type ? "return " : "",
            Option::MethodImplPrefix, cg.formatLabel(m_name).c_str(),
            cls);
  if (funcScope->isVariableArgument()) {
    cg_printf(", num_args");
  }
  if (m_params) {
    for (int i = 0; i < m_params->getCount(); i++) {
      ParameterExpressionPtr param =
        dynamic_pointer_cast<ParameterExpression>((*m_params)[i]);
      ASSERT(param);
      cg_printf(", %s%s", Option::VariablePrefix, param->getName().c_str());
    }
  }
  if (funcScope->isVariableArgument()) {
    cg_printf(", args");
  }
  cg_printf("); }\n");
  ar->popScope();
}


bool MethodStatement::outputFFI(CodeGenerator &cg, AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  ClassScopePtr clsScope = ar->getClassScope();
  bool pseudoMain = funcScope->inPseudoMain();
  bool inClass = !m_className.empty();
  // only expose public methods, and ignore those in redeclared classes
  bool inaccessible =
    inClass && (!m_modifiers->isPublic() || clsScope->isRedeclaring());
  // skip constructors
  bool isConstructor = inClass && funcScope->isConstructor(clsScope);
  bool valid = !pseudoMain && !inaccessible && !isConstructor;

  if (cg.getContext() == CodeGenerator::CppFFIDecl ||
      cg.getContext() == CodeGenerator::CppFFIImpl) {
    if (valid) outputCPPFFIStub(cg, ar);
    return true;
  }

  if (cg.getContext() == CodeGenerator::HsFFI) {
    if (valid) outputHSFFIStub(cg, ar);
    return true;
  }

  if (cg.getContext() == CodeGenerator::JavaFFI
   || cg.getContext() == CodeGenerator::JavaFFIInterface) {
    if (valid) outputJavaFFIStub(cg, ar);
    return true;
  }

  if (cg.getContext() == CodeGenerator::JavaFFICppDecl
   || cg.getContext() == CodeGenerator::JavaFFICppImpl) {
    if (valid) outputJavaFFICPPStub(cg, ar);
    return true;
  }

  if (cg.getContext() == CodeGenerator::SwigFFIDecl
   || cg.getContext() == CodeGenerator::SwigFFIImpl) {
    if (valid) outputSwigFFIStub(cg, ar);
    return true;
  }

  return false;
}

void MethodStatement::outputCPPFFIStub(CodeGenerator &cg,
                                       AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  bool varArgs = funcScope->isVariableArgument();
  bool ret = funcScope->getReturnType();
  string fname = funcScope->getId(cg);
  bool inClass = !m_className.empty();
  bool isStatic = !inClass || m_modifiers->isStatic();

  if (inClass && m_modifiers->isAbstract()) {
    return;
  }

  if (fname == "__lval" || fname == "__offsetget_lval") {
    return;
  }

  if (ret) {
    cg_printf("int");
  } else {
    cg_printf("void");
  }
  cg_printf(" %s%s%s(", Option::FFIFnPrefix,
            (inClass ? (m_className + "_cls_").c_str() : ""), fname.c_str());
  if (ret) {
    cg_printf("void** res");
  }

  bool first = !ret;

  if (!isStatic) {
    // instance methods need one additional parameter for the target
    if (first) {
      first = false;
    }
    else {
      cg_printf(", ");
    }
    cg_printf("Variant *target");
  }

  int ac = funcScope->getMaxParamCount();
  for (int i = 0; i < ac; ++i) {
    if (first) {
      first = false;
    } else {
      cg_printf(", ");
    }
    cg_printf("Variant *a%d", i);
  }
  if (varArgs) {
    if (!first) {
      cg_printf(", ");
    }
    cg_printf("Variant *va");
  }
  cg_printf(")");
  if (cg.getContext() == CodeGenerator::CppFFIDecl) {
    cg_printf(";\n");
  } else {
    cg_indentBegin(" {\n");
    if (ret) {
      cg_printf("return hphp_ffi_exportVariant(");
    }

    if (!inClass) {
      // simple function call
      cg_printf("%s%s(", Option::FunctionPrefix, fname.c_str());
    }
    else if (isStatic) {
      // static method call
      cg_printf("%s%s::%s%s(", Option::ClassPrefix, m_className.c_str(),
                Option::MethodPrefix, fname.c_str());
    }
    else {
      // instance method call
      cg_printf("dynamic_cast<%s%s *>(target->getObjectData())->",
                Option::ClassPrefix, m_className.c_str());
      cg_printf("%s%s(", Option::MethodPrefix, fname.c_str());
    }

    first = true;
    if (varArgs) {
      cg_printf("%d + (va->isNull() ? 0 : va->getArrayData()->size())", ac);
      first = false;
    }

    for (int i = 0; i < ac; ++i) {
      if (first) {
        first = false;
      } else {
        cg_printf(", ");
      }
      cg_printf("*a%d", i);
    }
    if (varArgs) {
      cg_printf(", va->toArray()");
    }
    if (ret) {
      cg_printf("), res");
    }
    cg_printf(");\n");
    cg_indentEnd("} /* function */\n");
  }
  return;
}

void MethodStatement::outputHSFFIStub(CodeGenerator &cg, AnalysisResultPtr ar) {
  if (!m_className.empty()) {
    // Haskell currently doesn't support FFI for class methods.
    return;
  }

  FunctionScopePtr funcScope = m_funcScope.lock();
  bool varArgs = funcScope->isVariableArgument();
  bool ret = funcScope->getReturnType();
  string fname = funcScope->getId(cg).c_str();
  cg_indentBegin("foreign import ccall \"stubs.h %s%s\" %s%s\n",
                 Option::FFIFnPrefix, fname.c_str(),
                 Option::FFIFnPrefix, fname.c_str());
  cg_printf(":: ");
  if (ret) {
    cg_printf("PtrPtr a -> ");
  }
  int ac = funcScope->getMaxParamCount();
  for (int i = 0; i < ac; ++i) {
    cg_printf("HphpVariantPtr -> ");
  }
  if (varArgs) {
    cg_printf("HphpVariantPtr -> ");
  }
  if (ret) {
    cg_printf("IO CInt");
  } else {
    cg_printf("IO ()");
  }
  cg_indentEnd("\n");

  cg_printf("f_%s :: ", fname.c_str());
  bool first = true;
  if (ac > 0) {
    cg_printf("(");
  }
  for (int i = 0; i < ac; ++i) {
    if (first) {
      first = false;
    } else {
      cg_printf(", ");
    }
    cg_printf("VariantAble a%d", i);
  }
  if (ac > 0) {
    cg_printf(") => ");
  }
  for (int i = 0; i < ac; ++i) {
    cg_printf("a%d -> ", i);
  }
  if (varArgs) {
    cg_printf("[Variant] -> ");
  }
  if (ret) {
    cg_printf("IO Variant");
  } else {
    cg_printf("IO ()");
  }
  cg_printf("\n");
  cg_printf("f_%s ", fname.c_str());
  for (int i = 0; i < ac; ++i) {
    cg_printf("v%d ", i);
  }
  if (varArgs) {
    cg_printf("va ");
  }
  cg_indentBegin("=%s\n", ret ? " alloca (\\pres ->" : "");
  for (int i = 0; i < ac; ++i) {
    cg_indentBegin("withExportedVariant (toVariant v%d) (\\p%d ->\n", i, i);
  }
  if (varArgs) {
    cg_indentBegin("withVParamList va (\\pva ->\n");
  }
  cg_indentBegin("do\n");
  cg_printf("%sffi_%s", ret ? "t <- " : "", fname.c_str());
  if (ret) {
    cg_printf(" pres");
  }
  for (int i = 0; i < ac; ++i) {
    cg_printf(" p%d", i);
  }
  if (varArgs) {
    cg_printf(" pva");
  }
  if (ret) {
    cg_printf("\n");
    cg_printf("ppres <- peek pres\n");
    cg_printf("buildVariant (fromIntegral t) ppres");
  }
  cg_indentEnd(""); // end do
  if (varArgs) {
    cg_indentEnd(")"); // end varargs
  }
  for (int i = 0; i < ac; ++i) {
    cg_indentEnd(")"); // end wEV i
  }
  if (ret) {
    cg_indentEnd(")"); // end alloca
  } else {
    cg_indentEnd("");
  }
  cg_printf("\n");
  return;
}

/**
 * Generates the Java stub method for a PHP toplevel function.
 *
 * @author qixin
 */
void MethodStatement::outputJavaFFIStub(CodeGenerator &cg,
                                        AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  bool varArgs = funcScope->isVariableArgument();
  bool ret = funcScope->getReturnType();
  bool inClass = !m_className.empty();
  bool isStatic = !inClass || m_modifiers->isStatic();
  string fname = funcScope->getId(cg);
  string originalName = funcScope->getOriginalName();
  if (originalName.length() < fname.length()) {
    // if there are functions of the same name, fname may contain "$$..."
    // in the end
    originalName += fname.substr(originalName.length());
  }

  if (originalName == "clone"     || originalName == "equals"
   || originalName == "finalize"  || originalName == "getClass"
   || originalName == "hashCode"  || originalName == "notify"
   || originalName == "notifyAll" || originalName == "toString"
   || originalName == "wait") {
    // not to clash with Java method names
    originalName = "_" + originalName;
  }

  if (cg.getContext() == CodeGenerator::JavaFFIInterface
   || inClass && m_modifiers->isAbstract()) {
    // skip all the abstract methods, because php overriding is not very
    // compatible with Java
    return;
  }

  if (!inClass) printSource(cg);

  // This Java method extracts the Variant pointer from the HphpVariant
  // argument as a 64-bit integer, and then calls the native version.
  bool exposeNative = false;
  int ac = funcScope->getMaxParamCount();
  if (ac > 0 || varArgs || !isStatic || !ret && inClass
   || cg.getContext() == CodeGenerator::JavaFFIInterface) {
    // make methods always return something, so that they can override
    // each other
    cg_printf("public %s%s %s(",
              (isStatic ? "static " : ""),
              (!ret && !inClass ? "void" : "HphpVariant"),
              originalName.c_str());
    ostringstream args;
    bool first = true;
    if (!isStatic) {
      // instance method has an additional parameter
      args << "this.getVariantPtr()";
    }
    for (int i = 0; i < ac; i++) {
      if (first) {
        first = false;
        if (!isStatic) args << ", ";
      }
      else {
        cg_printf(", ");
        args << ", ";
      }
      cg_printf("HphpVariant a%d", i);
      args << "a" << i << ".getVariantPtr()";
    }
    if (varArgs) {
      if (!first) {
        cg_printf(", ");
        args << ", ";
      }
      else if (!isStatic) {
        args << ", ";
      }
      cg_printf("HphpVariant va");
      args << "va.getVariantPtr()";
    }

    if (cg.getContext() == CodeGenerator::JavaFFIInterface) {
      cg_printf(");\n\n");
      return;
    }

    cg_indentBegin(") {\n");
    cg_printf("%s%s_native(%s);\n", (ret ? "return " : ""),
              originalName.c_str(),
              args.str().c_str());
    if (!ret && inClass) {
      cg_printf("return HphpNull.phpNull();\n");
    }
    cg_indentEnd("}\n\n");
  }
  else {
    exposeNative = true;
  }

  // the native method stub
  cg_printf("%s %snative %s %s%s(",
            (exposeNative ? "public" : "private"),
            (isStatic ? "static " : ""), (ret ? "HphpVariant" : "void"),
            originalName.c_str(),
            (exposeNative ? "" : "_native"));
  bool first = true;
  if (!isStatic) {
    // instance method has an additional parameter
    cg_printf("long targetPtr");
    first = false;
  }
  for (int i = 0; i < ac; i++) {
    if (first) first = false;
    else cg_printf(", ");
    cg_printf("long a%d", i);
  }
  if (varArgs) {
    if (!first) cg_printf(", ");
    cg_printf("long va");
  }
  cg_printf(");\n\n");
}

void MethodStatement::outputJavaFFICPPStub(CodeGenerator &cg,
                                           AnalysisResultPtr ar) {
  // TODO translate PHP namespace once that is supported
  string packageName = Option::JavaFFIRootPackage;

  FunctionScopePtr funcScope = m_funcScope.lock();
  bool varArgs = funcScope->isVariableArgument();
  bool ret = funcScope->getReturnType();
  bool inClass = !m_className.empty();
  bool isStatic = !inClass || m_modifiers->isStatic();
  string fname = funcScope->getId(cg);
  int ac = funcScope->getMaxParamCount();
  bool exposeNative = !(ac > 0 || varArgs || !isStatic || !ret && inClass);

  if (inClass && m_modifiers->isAbstract()) {
    // skip all the abstract methods, because hphp doesn't generate code
    // for them
    return;
  }

  if (fname == "__lval" || fname == "__offsetget_lval") return;

  const char *clsName;
  if (inClass) {
    // uses capitalized original class name
    ClassScopePtr cls = ar->findClass(m_className);
    clsName = cls->getOriginalName().c_str();
  }
  else {
    clsName = "HphpMain";
  }
  string mangledName = "Java." + packageName + "." + clsName + "." + fname
    + (exposeNative ? "" : "_native");
  // all the existing "_" are replaced as "_1"
  Util::replaceAll(mangledName, "_", "_1");
  Util::replaceAll(mangledName, ".", "_");

  cg_printf("JNIEXPORT %s JNICALL\n", ret ? "jobject" : "void");
  cg_printf("%s(JNIEnv *env, %s target", mangledName.c_str(),
            (isStatic ? "jclass" : "jobject"));

  ostringstream args;
  bool first = true;
  if (!isStatic) {
    // instance method also gets an additional argument, which is a Variant
    // pointer to the target, encoded in int64
    first = false;
    cg_printf(", jlong targetPtr");
    args << "(Variant *)targetPtr";
  }
  for (int i = 0; i < ac; i++) {
    cg_printf(", jlong a%d", i);
    if (first) first = false;
    else args << ", ";
    args << "(Variant *)a" << i;
  }
  if (varArgs) {
    cg_printf(", jlong va");
    if (!first) args << ", ";
    args << "(Variant *)va";
  }

  if (cg.getContext() == CodeGenerator::JavaFFICppDecl) {
    // java_stubs.h
    cg_printf(");\n\n");
    return;
  }

  cg_indentBegin(") {\n");

  // support static/instance methods
  if (ret) {
    cg_printf("void *result;\n");
    cg_printf("int kind = ");
    cg_printf("%s%s%s(&result",
              Option::FFIFnPrefix,
              (inClass ? (m_className + "_cls_").c_str() : ""), fname.c_str());
    if (!isStatic || ac > 0 || varArgs) cg_printf(", ");
  }
  else {
    cg_printf("%s%s%s(", Option::FFIFnPrefix,
                         (inClass ? (m_className + "_cls_").c_str() : ""),
                         fname.c_str());
  }
  cg_printf("%s);\n", args.str().c_str());
  if (ret) {
    if (!inClass) {
      // HphpMain extends hphp.Hphp.
      cg_printf("jclass hphp = env->GetSuperclass(target);\n");
    }
    else {
      cg_printf("jclass hphp = env->FindClass(\"hphp/Hphp\");\n");
    }
    cg_printf("return exportVariantToJava(env, hphp, result, kind);\n");
  }

  cg_indentEnd("} /* function */\n\n");
}

void MethodStatement::outputSwigFFIStub(CodeGenerator &cg,
                                        AnalysisResultPtr ar) {
  FunctionScopePtr funcScope = m_funcScope.lock();
  bool varArgs = funcScope->isVariableArgument();
  bool ret = funcScope->getReturnType();
  string fname = funcScope->getId(cg);
  string originalName = funcScope->getOriginalName();
  int ac = funcScope->getMaxParamCount();

  if (cg.getContext() == CodeGenerator::SwigFFIImpl) {
    printSource(cg);
  }

  cg_printf("Variant *%s(HphpSession *s", originalName.c_str());
  ostringstream args;
  bool first = true;
  for (int i = 0; i < ac; i++) {
    cg_printf(", Variant *a%d", i);
    if (first) first = false;
    else args << ", ";
    args << "a" << i;
  }
  if (varArgs) {
    cg_printf(", Variant *va");
    if (!first) args << ", ";
    args << "va";
  }

  if (cg.getContext() == CodeGenerator::SwigFFIDecl) {
    cg_printf(");\n\n");
    return;
  }

  cg_indentBegin(") {\n");
  if (ret) {
    cg_printf("void *result;\n");
    cg_printf("int kind = ");
    cg_printf("%s%s(&result", Option::FFIFnPrefix, fname.c_str());
    if (ac > 0 || varArgs) cg_printf(", ");
  } else {
    cg_printf("%s%s(", Option::FFIFnPrefix, fname.c_str());
  }
  cg_printf("%s);\n", args.str().c_str());
  cg_printf("Variant *ret = ");
  if (ret) {
    cg_printf("hphpBuildVariant(kind, result);\n");
    cg_printf("s->addVariant(ret);\n");
  } else {
    cg_printf("hphpBuildVariant(0, 0);\n");
    cg_printf("s->addVariant(ret);\n");
  }
  cg_printf("return ret;\n");
  cg_indentEnd("}\n\n");
}
