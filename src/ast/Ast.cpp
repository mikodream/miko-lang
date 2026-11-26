#include "miko/ast/Ast.h"

#include <utility>

namespace miko {

namespace {

void writeIndent(std::ostream& out, int indent) {
    for (int i = 0; i < indent; ++i) {
        out << ' ';
    }
}

} // namespace

AstNode::AstNode(SourceLocation location) : location_(std::move(location)) {
}

const SourceLocation& AstNode::location() const {
    return location_;
}

void AstNode::setLocation(SourceLocation location) {
    location_ = std::move(location);
}

IntegerExpr::IntegerExpr(std::string value) : value_(std::move(value)) {
}

void IntegerExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "IntegerExpr " << value_ << '\n';
}

const std::string& IntegerExpr::value() const {
    return value_;
}

FloatExpr::FloatExpr(std::string value) : value_(std::move(value)) {
}

void FloatExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "FloatExpr " << value_ << '\n';
}

const std::string& FloatExpr::value() const {
    return value_;
}

StringExpr::StringExpr(std::string value) : value_(std::move(value)) {
}

void StringExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "StringExpr " << value_ << '\n';
}

const std::string& StringExpr::value() const {
    return value_;
}

CharExpr::CharExpr(std::string value) : value_(std::move(value)) {
}

void CharExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "CharExpr " << value_ << '\n';
}

const std::string& CharExpr::value() const {
    return value_;
}

BoolExpr::BoolExpr(bool value) : value_(value) {
}

void BoolExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "BoolExpr " << (value_ ? "true" : "false") << '\n';
}

bool BoolExpr::value() const {
    return value_;
}

VariableExpr::VariableExpr(std::string name) : name_(std::move(name)) {
}

void VariableExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "VariableExpr " << name_ << '\n';
}

const std::string& VariableExpr::name() const {
    return name_;
}

void VariableExpr::setName(std::string name) {
    name_ = std::move(name);
}

CallExpr::CallExpr(std::string callee, std::vector<ExprPtr> arguments)
    : callee_(std::move(callee)), arguments_(std::move(arguments)) {
}

void CallExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "CallExpr " << callee_ << '\n';
    for (const auto& argument : arguments_) {
        argument->dump(out, indent + 2);
    }
}

const std::string& CallExpr::callee() const {
    return callee_;
}

void CallExpr::setCallee(std::string callee) {
    callee_ = std::move(callee);
}

const std::vector<ExprPtr>& CallExpr::arguments() const {
    return arguments_;
}

std::vector<ExprPtr>& CallExpr::arguments() {
    return arguments_;
}

BinaryExpr::BinaryExpr(std::string op, ExprPtr left, ExprPtr right)
    : op_(std::move(op)), left_(std::move(left)), right_(std::move(right)) {
}

void BinaryExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "BinaryExpr " << op_ << '\n';
    left_->dump(out, indent + 2);
    right_->dump(out, indent + 2);
}

const std::string& BinaryExpr::op() const {
    return op_;
}

const Expr& BinaryExpr::left() const {
    return *left_;
}

const Expr& BinaryExpr::right() const {
    return *right_;
}

UnaryExpr::UnaryExpr(std::string op, ExprPtr operand)
    : op_(std::move(op)), operand_(std::move(operand)) {
}

void UnaryExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "UnaryExpr " << op_ << '\n';
    operand_->dump(out, indent + 2);
}

const std::string& UnaryExpr::op() const {
    return op_;
}

const Expr& UnaryExpr::operand() const {
    return *operand_;
}

ArrayExpr::ArrayExpr(std::vector<ExprPtr> elements) : elements_(std::move(elements)) {
}

void ArrayExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "ArrayExpr\n";
    for (const auto& element : elements_) {
        element->dump(out, indent + 2);
    }
}

const std::vector<ExprPtr>& ArrayExpr::elements() const {
    return elements_;
}

IndexExpr::IndexExpr(ExprPtr target, ExprPtr index)
    : target_(std::move(target)), index_(std::move(index)) {
}

void IndexExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "IndexExpr\n";
    writeIndent(out, indent + 2);
    out << "Target\n";
    target_->dump(out, indent + 4);
    writeIndent(out, indent + 2);
    out << "Index\n";
    index_->dump(out, indent + 4);
}

const Expr& IndexExpr::target() const {
    return *target_;
}

const Expr& IndexExpr::index() const {
    return *index_;
}

FieldAccessExpr::FieldAccessExpr(ExprPtr target, std::string field)
    : target_(std::move(target)), field_(std::move(field)) {
}

void FieldAccessExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "FieldAccessExpr ." << field_ << '\n';
    target_->dump(out, indent + 2);
}

const Expr& FieldAccessExpr::target() const {
    return *target_;
}

const std::string& FieldAccessExpr::field() const {
    return field_;
}

StructLiteralExpr::StructLiteralExpr(std::string typeName, std::vector<StructFieldInit> fields)
    : typeName_(std::move(typeName)), fields_(std::move(fields)) {
}

void StructLiteralExpr::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "StructLiteralExpr " << typeName_ << '\n';
    for (const auto& field : fields_) {
        writeIndent(out, indent + 2);
        out << field.first << '\n';
        field.second->dump(out, indent + 4);
    }
}

const std::string& StructLiteralExpr::typeName() const {
    return typeName_;
}

void StructLiteralExpr::setTypeName(std::string typeName) {
    typeName_ = std::move(typeName);
}

const std::vector<StructFieldInit>& StructLiteralExpr::fields() const {
    return fields_;
}

std::vector<StructFieldInit>& StructLiteralExpr::fields() {
    return fields_;
}

LetStmt::LetStmt(std::string name, std::string explicitType, ExprPtr initializer, bool isConst)
    : name_(std::move(name)),
      explicitType_(std::move(explicitType)),
      initializer_(std::move(initializer)),
      isConst_(isConst) {
}

void LetStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << (isConst_ ? "ConstStmt " : "LetStmt ") << name_;
    if (!explicitType_.empty()) {
        out << ": " << explicitType_;
    }
    out << '\n';
    if (initializer_) {
        initializer_->dump(out, indent + 2);
    }
}

const std::string& LetStmt::name() const {
    return name_;
}

const std::string& LetStmt::explicitType() const {
    return explicitType_;
}

void LetStmt::setExplicitType(std::string explicitType) {
    explicitType_ = std::move(explicitType);
}

const Expr* LetStmt::initializer() const {
    return initializer_.get();
}

bool LetStmt::isConst() const {
    return isConst_;
}

AssignStmt::AssignStmt(std::string name, ExprPtr value) : name_(std::move(name)), value_(std::move(value)) {
}

void AssignStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "AssignStmt " << name_ << '\n';
    value_->dump(out, indent + 2);
}

const std::string& AssignStmt::name() const {
    return name_;
}

const Expr& AssignStmt::value() const {
    return *value_;
}

IndexAssignStmt::IndexAssignStmt(std::string name, ExprPtr index, ExprPtr value)
    : name_(std::move(name)), index_(std::move(index)), value_(std::move(value)) {
}

void IndexAssignStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "IndexAssignStmt " << name_ << '\n';
    writeIndent(out, indent + 2);
    out << "Index\n";
    index_->dump(out, indent + 4);
    writeIndent(out, indent + 2);
    out << "Value\n";
    value_->dump(out, indent + 4);
}

const std::string& IndexAssignStmt::name() const {
    return name_;
}

const Expr& IndexAssignStmt::index() const {
    return *index_;
}

const Expr& IndexAssignStmt::value() const {
    return *value_;
}

ReturnStmt::ReturnStmt(ExprPtr value) : value_(std::move(value)) {
}

void ReturnStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "ReturnStmt\n";
    if (value_) {
        value_->dump(out, indent + 2);
    }
}

const Expr* ReturnStmt::value() const {
    return value_.get();
}

ExprStmt::ExprStmt(ExprPtr expression) : expression_(std::move(expression)) {
}

void ExprStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "ExprStmt\n";
    expression_->dump(out, indent + 2);
}

const Expr& ExprStmt::expression() const {
    return *expression_;
}

BlockStmt::BlockStmt(std::vector<StmtPtr> statements) : statements_(std::move(statements)) {
}

void BlockStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "BlockStmt\n";
    for (const auto& statement : statements_) {
        statement->dump(out, indent + 2);
    }
}

const std::vector<StmtPtr>& BlockStmt::statements() const {
    return statements_;
}

IfStmt::IfStmt(ExprPtr condition, std::unique_ptr<BlockStmt> thenBlock, std::unique_ptr<BlockStmt> elseBlock)
    : condition_(std::move(condition)), thenBlock_(std::move(thenBlock)), elseBlock_(std::move(elseBlock)) {
}

void IfStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "IfStmt\n";
    writeIndent(out, indent + 2);
    out << "Condition\n";
    condition_->dump(out, indent + 4);
    writeIndent(out, indent + 2);
    out << "Then\n";
    thenBlock_->dump(out, indent + 4);
    if (elseBlock_) {
        writeIndent(out, indent + 2);
        out << "Else\n";
        elseBlock_->dump(out, indent + 4);
    }
}

const Expr& IfStmt::condition() const {
    return *condition_;
}

const BlockStmt& IfStmt::thenBlock() const {
    return *thenBlock_;
}

const BlockStmt* IfStmt::elseBlock() const {
    return elseBlock_.get();
}

WhileStmt::WhileStmt(ExprPtr condition, std::unique_ptr<BlockStmt> body)
    : condition_(std::move(condition)), body_(std::move(body)) {
}

void WhileStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "WhileStmt\n";
    writeIndent(out, indent + 2);
    out << "Condition\n";
    condition_->dump(out, indent + 4);
    writeIndent(out, indent + 2);
    out << "Body\n";
    body_->dump(out, indent + 4);
}

const Expr& WhileStmt::condition() const {
    return *condition_;
}

const BlockStmt& WhileStmt::body() const {
    return *body_;
}

ForStmt::ForStmt(StmtPtr initializer, ExprPtr condition, StmtPtr increment, std::unique_ptr<BlockStmt> body)
    : initializer_(std::move(initializer)),
      condition_(std::move(condition)),
      increment_(std::move(increment)),
      body_(std::move(body)) {
}

void ForStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "ForStmt\n";
    if (initializer_) {
        writeIndent(out, indent + 2);
        out << "Initializer\n";
        initializer_->dump(out, indent + 4);
    }
    if (condition_) {
        writeIndent(out, indent + 2);
        out << "Condition\n";
        condition_->dump(out, indent + 4);
    }
    if (increment_) {
        writeIndent(out, indent + 2);
        out << "Increment\n";
        increment_->dump(out, indent + 4);
    }
    writeIndent(out, indent + 2);
    out << "Body\n";
    body_->dump(out, indent + 4);
}

const Stmt* ForStmt::initializer() const {
    return initializer_.get();
}

const Expr* ForStmt::condition() const {
    return condition_.get();
}

const Stmt* ForStmt::increment() const {
    return increment_.get();
}

const BlockStmt& ForStmt::body() const {
    return *body_;
}

void BreakStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "BreakStmt\n";
}

void ContinueStmt::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "ContinueStmt\n";
}

Parameter::Parameter(std::string name, std::string type) : name_(std::move(name)), type_(std::move(type)) {
}

void Parameter::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "Parameter " << name_ << ": " << type_ << '\n';
}

const std::string& Parameter::name() const {
    return name_;
}

const std::string& Parameter::type() const {
    return type_;
}

void Parameter::setType(std::string type) {
    type_ = std::move(type);
}

ImportDecl::ImportDecl(std::string path, std::string alias, bool isStringPath)
    : path_(std::move(path)), alias_(std::move(alias)), isStringPath_(isStringPath) {
}

void ImportDecl::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "ImportDecl " << (isStringPath_ ? "\"" : "") << path_ << (isStringPath_ ? "\"" : "") << '\n';
    if (!alias_.empty()) {
        writeIndent(out, indent + 2);
        out << "Alias " << alias_ << '\n';
    }
}

const std::string& ImportDecl::path() const {
    return path_;
}

const std::string& ImportDecl::alias() const {
    return alias_;
}

bool ImportDecl::isStringPath() const {
    return isStringPath_;
}

StructDecl::StructDecl(std::string name, std::vector<Parameter> fields, bool isPublic)
    : name_(std::move(name)), fields_(std::move(fields)), isPublic_(isPublic) {
}

void StructDecl::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    if (isPublic_) {
        out << "Public ";
    }
    out << "StructDecl " << name_ << '\n';
    for (const auto& field : fields_) {
        field.dump(out, indent + 2);
    }
}

const std::string& StructDecl::name() const {
    return name_;
}

void StructDecl::setName(std::string name) {
    name_ = std::move(name);
}

const std::vector<Parameter>& StructDecl::fields() const {
    return fields_;
}

std::vector<Parameter>& StructDecl::fields() {
    return fields_;
}

bool StructDecl::isPublic() const {
    return isPublic_;
}

void StructDecl::setPublic(bool isPublic) {
    isPublic_ = isPublic;
}

EnumDecl::EnumDecl(std::string name, std::vector<std::string> members, bool isPublic)
    : name_(std::move(name)), members_(std::move(members)), isPublic_(isPublic) {
}

void EnumDecl::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "EnumDecl " << name_ << " (";
    for (std::size_t i = 0; i < members_.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << members_[i];
    }
    out << ")\n";
}

const std::string& EnumDecl::name() const {
    return name_;
}

void EnumDecl::setName(std::string name) {
    name_ = std::move(name);
}

const std::vector<std::string>& EnumDecl::members() const {
    return members_;
}

bool EnumDecl::isPublic() const {
    return isPublic_;
}

void EnumDecl::setPublic(bool isPublic) {
    isPublic_ = isPublic;
}

FunctionDecl::FunctionDecl(std::string name, std::vector<Parameter> parameters, std::string returnType, std::unique_ptr<BlockStmt> body, bool isPublic)
    : name_(std::move(name)),
      parameters_(std::move(parameters)),
      returnType_(std::move(returnType)),
      body_(std::move(body)),
      isPublic_(isPublic) {
}

void FunctionDecl::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    if (isPublic_) {
        out << "Public ";
    }
    out << "FunctionDecl " << name_;
    if (!returnType_.empty()) {
        out << " -> " << returnType_;
    }
    out << '\n';

    for (const auto& parameter : parameters_) {
        parameter.dump(out, indent + 2);
    }

    if (body_) {
        body_->dump(out, indent + 2);
    }
}

const std::string& FunctionDecl::name() const {
    return name_;
}

void FunctionDecl::setName(std::string name) {
    name_ = std::move(name);
}

const std::vector<Parameter>& FunctionDecl::parameters() const {
    return parameters_;
}

std::vector<Parameter>& FunctionDecl::parameters() {
    return parameters_;
}

const std::string& FunctionDecl::returnType() const {
    return returnType_;
}

void FunctionDecl::setReturnType(std::string returnType) {
    returnType_ = std::move(returnType);
}

const BlockStmt& FunctionDecl::body() const {
    return *body_;
}

BlockStmt& FunctionDecl::body() {
    return *body_;
}

bool FunctionDecl::isPublic() const {
    return isPublic_;
}

void FunctionDecl::setPublic(bool isPublic) {
    isPublic_ = isPublic;
}

Program::Program(std::vector<std::unique_ptr<FunctionDecl>> functions) : functions_(std::move(functions)) {
}

Program::Program(std::vector<std::unique_ptr<ImportDecl>> imports,
                 std::vector<std::unique_ptr<StructDecl>> structs,
                 std::vector<std::unique_ptr<FunctionDecl>> functions)
    : imports_(std::move(imports)), structs_(std::move(structs)), functions_(std::move(functions)) {
}

Program::Program(std::vector<std::unique_ptr<ImportDecl>> imports,
                 std::vector<std::unique_ptr<StructDecl>> structs,
                 std::vector<std::unique_ptr<EnumDecl>> enums,
                 std::vector<std::unique_ptr<FunctionDecl>> functions)
    : imports_(std::move(imports)),
      structs_(std::move(structs)),
      enums_(std::move(enums)),
      functions_(std::move(functions)) {
}

void Program::dump(std::ostream& out, int indent) const {
    writeIndent(out, indent);
    out << "Program\n";
    for (const auto& import : imports_) {
        import->dump(out, indent + 2);
    }
    for (const auto& structure : structs_) {
        structure->dump(out, indent + 2);
    }
    for (const auto& enumeration : enums_) {
        enumeration->dump(out, indent + 2);
    }
    for (const auto& function : functions_) {
        function->dump(out, indent + 2);
    }
}

const std::vector<std::unique_ptr<ImportDecl>>& Program::imports() const {
    return imports_;
}

const std::vector<std::unique_ptr<StructDecl>>& Program::structs() const {
    return structs_;
}

const std::vector<std::unique_ptr<EnumDecl>>& Program::enums() const {
    return enums_;
}

const std::vector<std::unique_ptr<FunctionDecl>>& Program::functions() const {
    return functions_;
}

std::vector<std::unique_ptr<StructDecl>> Program::takeStructs() {
    return std::move(structs_);
}

std::vector<std::unique_ptr<EnumDecl>> Program::takeEnums() {
    return std::move(enums_);
}

std::vector<std::unique_ptr<FunctionDecl>> Program::takeFunctions() {
    return std::move(functions_);
}

void Program::addEnum(std::unique_ptr<EnumDecl> enumeration) {
    enums_.push_back(std::move(enumeration));
}

} // namespace miko
