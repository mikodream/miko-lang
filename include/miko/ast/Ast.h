#pragma once

#include "miko/SourceLocation.h"

#include <memory>
#include <ostream>
#include <utility>
#include <string>
#include <vector>

namespace miko {

class AstNode {
public:
    AstNode() = default;
    explicit AstNode(SourceLocation location);
    virtual ~AstNode() = default;
    virtual void dump(std::ostream& out, int indent = 0) const = 0;
    const SourceLocation& location() const;
    void setLocation(SourceLocation location);

private:
    SourceLocation location_;
};

class Expr : public AstNode {
};

class Stmt : public AstNode {
};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using StructFieldInit = std::pair<std::string, ExprPtr>;

class IntegerExpr final : public Expr {
public:
    explicit IntegerExpr(std::string value);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& value() const;

private:
    std::string value_;
};

class FloatExpr final : public Expr {
public:
    explicit FloatExpr(std::string value);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& value() const;

private:
    std::string value_;
};

class StringExpr final : public Expr {
public:
    explicit StringExpr(std::string value);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& value() const;

private:
    std::string value_;
};

class CharExpr final : public Expr {
public:
    explicit CharExpr(std::string value);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& value() const;

private:
    std::string value_;
};

class BoolExpr final : public Expr {
public:
    explicit BoolExpr(bool value);
    void dump(std::ostream& out, int indent = 0) const override;
    bool value() const;

private:
    bool value_;
};

class VariableExpr final : public Expr {
public:
    explicit VariableExpr(std::string name);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    void setName(std::string name);

private:
    std::string name_;
};

class CallExpr final : public Expr {
public:
    CallExpr(std::string callee, std::vector<ExprPtr> arguments);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& callee() const;
    void setCallee(std::string callee);
    const std::vector<ExprPtr>& arguments() const;
    std::vector<ExprPtr>& arguments();

private:
    std::string callee_;
    std::vector<ExprPtr> arguments_;
};

class BinaryExpr final : public Expr {
public:
    BinaryExpr(std::string op, ExprPtr left, ExprPtr right);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& op() const;
    const Expr& left() const;
    const Expr& right() const;

private:
    std::string op_;
    ExprPtr left_;
    ExprPtr right_;
};

class UnaryExpr final : public Expr {
public:
    UnaryExpr(std::string op, ExprPtr operand);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& op() const;
    const Expr& operand() const;

private:
    std::string op_;
    ExprPtr operand_;
};

class ArrayExpr final : public Expr {
public:
    explicit ArrayExpr(std::vector<ExprPtr> elements);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::vector<ExprPtr>& elements() const;

private:
    std::vector<ExprPtr> elements_;
};

class IndexExpr final : public Expr {
public:
    IndexExpr(ExprPtr target, ExprPtr index);
    void dump(std::ostream& out, int indent = 0) const override;
    const Expr& target() const;
    const Expr& index() const;

private:
    ExprPtr target_;
    ExprPtr index_;
};

class FieldAccessExpr final : public Expr {
public:
    FieldAccessExpr(ExprPtr target, std::string field);
    void dump(std::ostream& out, int indent = 0) const override;
    const Expr& target() const;
    const std::string& field() const;

private:
    ExprPtr target_;
    std::string field_;
};

class StructLiteralExpr final : public Expr {
public:
    StructLiteralExpr(std::string typeName, std::vector<StructFieldInit> fields);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& typeName() const;
    void setTypeName(std::string typeName);
    const std::vector<StructFieldInit>& fields() const;
    std::vector<StructFieldInit>& fields();

private:
    std::string typeName_;
    std::vector<StructFieldInit> fields_;
};

class LetStmt final : public Stmt {
public:
    LetStmt(std::string name, std::string explicitType, ExprPtr initializer, bool isConst);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    const std::string& explicitType() const;
    void setExplicitType(std::string explicitType);
    const Expr* initializer() const;
    bool isConst() const;

private:
    std::string name_;
    std::string explicitType_;
    ExprPtr initializer_;
    bool isConst_ = false;
};

class AssignStmt final : public Stmt {
public:
    AssignStmt(std::string name, ExprPtr value);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    const Expr& value() const;

private:
    std::string name_;
    ExprPtr value_;
};

class IndexAssignStmt final : public Stmt {
public:
    IndexAssignStmt(std::string name, ExprPtr index, ExprPtr value);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    const Expr& index() const;
    const Expr& value() const;

private:
    std::string name_;
    ExprPtr index_;
    ExprPtr value_;
};

class ReturnStmt final : public Stmt {
public:
    explicit ReturnStmt(ExprPtr value);
    void dump(std::ostream& out, int indent = 0) const override;
    const Expr* value() const;

private:
    ExprPtr value_;
};

class ExprStmt final : public Stmt {
public:
    explicit ExprStmt(ExprPtr expression);
    void dump(std::ostream& out, int indent = 0) const override;
    const Expr& expression() const;

private:
    ExprPtr expression_;
};

class BlockStmt final : public Stmt {
public:
    explicit BlockStmt(std::vector<StmtPtr> statements);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::vector<StmtPtr>& statements() const;

private:
    std::vector<StmtPtr> statements_;
};

class IfStmt final : public Stmt {
public:
    IfStmt(ExprPtr condition, std::unique_ptr<BlockStmt> thenBlock, std::unique_ptr<BlockStmt> elseBlock);
    void dump(std::ostream& out, int indent = 0) const override;
    const Expr& condition() const;
    const BlockStmt& thenBlock() const;
    const BlockStmt* elseBlock() const;

private:
    ExprPtr condition_;
    std::unique_ptr<BlockStmt> thenBlock_;
    std::unique_ptr<BlockStmt> elseBlock_;
};

class WhileStmt final : public Stmt {
public:
    WhileStmt(ExprPtr condition, std::unique_ptr<BlockStmt> body);
    void dump(std::ostream& out, int indent = 0) const override;
    const Expr& condition() const;
    const BlockStmt& body() const;

private:
    ExprPtr condition_;
    std::unique_ptr<BlockStmt> body_;
};

class ForStmt final : public Stmt {
public:
    ForStmt(StmtPtr initializer, ExprPtr condition, StmtPtr increment, std::unique_ptr<BlockStmt> body);
    void dump(std::ostream& out, int indent = 0) const override;
    const Stmt* initializer() const;
    const Expr* condition() const;
    const Stmt* increment() const;
    const BlockStmt& body() const;

private:
    StmtPtr initializer_;
    ExprPtr condition_;
    StmtPtr increment_;
    std::unique_ptr<BlockStmt> body_;
};

class BreakStmt final : public Stmt {
public:
    void dump(std::ostream& out, int indent = 0) const override;
};

class ContinueStmt final : public Stmt {
public:
    void dump(std::ostream& out, int indent = 0) const override;
};

class Parameter final {
public:
    Parameter(std::string name, std::string type);
    void dump(std::ostream& out, int indent = 0) const;
    const std::string& name() const;
    const std::string& type() const;
    void setType(std::string type);

private:
    std::string name_;
    std::string type_;
};

class ImportDecl final : public AstNode {
public:
    ImportDecl(std::string path, std::string alias, bool isStringPath);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& path() const;
    const std::string& alias() const;
    bool isStringPath() const;

private:
    std::string path_;
    std::string alias_;
    bool isStringPath_ = false;
};

class StructDecl final : public AstNode {
public:
    StructDecl(std::string name, std::vector<Parameter> fields, bool isPublic = false);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    void setName(std::string name);
    const std::vector<Parameter>& fields() const;
    std::vector<Parameter>& fields();
    bool isPublic() const;
    void setPublic(bool isPublic);

private:
    std::string name_;
    std::vector<Parameter> fields_;
    bool isPublic_ = false;
};

class EnumDecl final : public AstNode {
public:
    EnumDecl(std::string name, std::vector<std::string> members, bool isPublic = false);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    void setName(std::string name);
    const std::vector<std::string>& members() const;
    bool isPublic() const;
    void setPublic(bool isPublic);

private:
    std::string name_;
    std::vector<std::string> members_;
    bool isPublic_ = false;
};

class FunctionDecl final : public AstNode {
public:
    FunctionDecl(std::string name, std::vector<Parameter> parameters, std::string returnType, std::unique_ptr<BlockStmt> body, bool isPublic = false);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::string& name() const;
    void setName(std::string name);
    const std::vector<Parameter>& parameters() const;
    std::vector<Parameter>& parameters();
    const std::string& returnType() const;
    void setReturnType(std::string returnType);
    const BlockStmt& body() const;
    BlockStmt& body();
    bool isPublic() const;
    void setPublic(bool isPublic);

private:
    std::string name_;
    std::vector<Parameter> parameters_;
    std::string returnType_;
    std::unique_ptr<BlockStmt> body_;
    bool isPublic_ = false;
};

class Program final : public AstNode {
public:
    explicit Program(std::vector<std::unique_ptr<FunctionDecl>> functions);
    Program(std::vector<std::unique_ptr<ImportDecl>> imports,
            std::vector<std::unique_ptr<StructDecl>> structs,
            std::vector<std::unique_ptr<FunctionDecl>> functions);
    Program(std::vector<std::unique_ptr<ImportDecl>> imports,
            std::vector<std::unique_ptr<StructDecl>> structs,
            std::vector<std::unique_ptr<EnumDecl>> enums,
            std::vector<std::unique_ptr<FunctionDecl>> functions);
    void dump(std::ostream& out, int indent = 0) const override;
    const std::vector<std::unique_ptr<ImportDecl>>& imports() const;
    const std::vector<std::unique_ptr<StructDecl>>& structs() const;
    const std::vector<std::unique_ptr<EnumDecl>>& enums() const;
    const std::vector<std::unique_ptr<FunctionDecl>>& functions() const;
    std::vector<std::unique_ptr<StructDecl>> takeStructs();
    std::vector<std::unique_ptr<EnumDecl>> takeEnums();
    std::vector<std::unique_ptr<FunctionDecl>> takeFunctions();
    void addEnum(std::unique_ptr<EnumDecl> enumeration);

private:
    std::vector<std::unique_ptr<ImportDecl>> imports_;
    std::vector<std::unique_ptr<StructDecl>> structs_;
    std::vector<std::unique_ptr<EnumDecl>> enums_;
    std::vector<std::unique_ptr<FunctionDecl>> functions_;
};

} // namespace miko
