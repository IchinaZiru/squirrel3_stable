#include "sqpcheader.h"
#ifndef NO_COMPILER
#include <stdarg.h>
#include <setjmp.h>
#include "sqopcodes.h"
#include "sqstring.h"
#include "sqfuncproto.h"
#include "sqcompiler.h"
#include "sqfuncstate.h"
#include "sqlexer.h"
#include "sqvm.h"
#include "sqtable.h"

#define EXPR   1
#define OBJECT 2
#define BASE   3
#define LOCAL  4
#define OUTER  5

/**
 * @brief 式の状態を管理する構造体。
 * @details コンパイラが式を評価する際の、式の種類やスタック上の位置などの状態を保持します。
 */
struct SQExpState {
  /**
   * @brief 式の型。
   * @details EXPR, OBJECT, BASE, OUTER, LOCALのいずれかの値を取ります。
   */
  SQInteger  etype;       /* expr. type; one of EXPR, OBJECT, BASE, OUTER or LOCAL */
  /**
   * @brief スタック上の式の位置。
   * @details OBJECT型とBASE型の場合は -1 になります。
   */
  SQInteger  epos;        /* expr. location on stack; -1 for OBJECT and BASE */
  /**
   * @brief デリファレンスを抑制するフラグ。
   * @details trueの場合、次の値のデリファレンスを行いません。
   */
  bool       donot_get;   /* signal not to deref the next value */
};

/**
 * @brief コンパイラエラーメッセージの最大長。
 */
#define MAX_COMPILER_ERROR_LEN 256

/**
 * @brief コンパイル時のスコープ情報を保持する構造体。
 * @details 現在のスコープのアウター変数の数とスタックサイズを管理します。
 */
struct SQScope {
    SQInteger outers;     //!< アウター変数の数
    SQInteger stacksize;  //!< スタックサイズ
};

/**
 * @brief 新しいスコープを開始するためのマクロ。
 * @details 現在のスコープ情報を保存し、新しいスコープのスタックサイズとアウター変数の数を初期化します。
 */
#define BEGIN_SCOPE() SQScope __oldscope__ = _scope; \
                     _scope.outers = _fs->_outers; \
                     _scope.stacksize = _fs->GetStackSize();

/**
 * @brief アウター変数を解決するためのマクロ。
 * @details 現在のスコープを抜ける際に、必要であればクローズ命令(_OP_CLOSE)を追加します。
 */
#define RESOLVE_OUTERS() if(_fs->GetStackSize() != _scope.stacksize) { \
                            if(_fs->CountOuters(_scope.stacksize)) { \
                                _fs->AddInstruction(_OP_CLOSE,0,_scope.stacksize); \
                            } \
                        }

/**
 * @brief スコープを終了するマクロ（クローズ命令なし）。
 * @details スタックサイズをスコープ開始時の状態に戻し、スコープ情報を復元します。
 * アウター変数の解決（クローズ命令の生成）は行いません。
 */
#define END_SCOPE_NO_CLOSE() {  if(_fs->GetStackSize() != _scope.stacksize) { \
                            _fs->SetStackSize(_scope.stacksize); \
                        } \
                        _scope = __oldscope__; \
                    }

/**
 * @brief スコープを終了するマクロ。
 * @details スタックサイズをスコープ開始時の状態に戻します。
 * スコープ内でアウター変数の数が変化した場合は、クローズ命令(_OP_CLOSE)を生成します。
 * 最後にスコープ情報を復元します。
 */
#define END_SCOPE() {   SQInteger oldouters = _fs->_outers;\
                        if(_fs->GetStackSize() != _scope.stacksize) { \
                            _fs->SetStackSize(_scope.stacksize); \
                            if(oldouters != _fs->_outers) { \
                                _fs->AddInstruction(_OP_CLOSE,0,_scope.stacksize); \
                            } \
                        } \
                        _scope = __oldscope__; \
                    }

/**
 * @brief break/continueが可能なブロックを開始するためのマクロ。
 * @details ループなどのブロックの開始時に、未解決のbreak/continueの数を保存し、
 * 新しいbreak/continueターゲットをスタックに追加します。
 */
#define BEGIN_BREAKBLE_BLOCK()  SQInteger __nbreaks__=_fs->_unresolvedbreaks.size(); \
                            SQInteger __ncontinues__=_fs->_unresolvedcontinues.size(); \
                            _fs->_breaktargets.push_back(0);_fs->_continuetargets.push_back(0);

/**
 * @brief break/continueが可能なブロックを終了するためのマクロ。
 * @details ブロック内で追加された未解決のbreakおよびcontinueを解決します。
 * その後、break/continueターゲットをスタックからポップします。
 * @param continue_target continue文のジャンプ先となる命令の位置。
 */
#define END_BREAKBLE_BLOCK(continue_target) {__nbreaks__=_fs->_unresolvedbreaks.size()-__nbreaks__; \
                    __ncontinues__=_fs->_unresolvedcontinues.size()-__ncontinues__; \
                    if(__ncontinues__>0)ResolveContinues(_fs,__ncontinues__,continue_target); \
                    if(__nbreaks__>0)ResolveBreaks(_fs,__nbreaks__); \
                    _fs->_breaktargets.pop_back();_fs->_continuetargets.pop_back();}

/**
 * @brief Squirrelスクリプトをバイトコードにコンパイルするクラス。
 * @details 字句解析、構文解析、中間コード生成の全プロセスを管理します。
 */
class SQCompiler
{
public:
    /**
     * @brief SQCompilerのコンストラクタ。
     * @details コンパイラの初期化を行います。
     * @param v 関連付けられるSquirrel VMへのポインタ。
     * @param rg 字句解析器がソースコードを読み込むための関数ポインタ。
     * @param up rg関数に渡されるユーザーポインタ。
     * @param sourcename コンパイル対象のソース名。
     * @param raiseerror エラー発生時にエラーハンドラを呼び出すかどうか。
     * @param lineinfo バイトコードに行情報を含めるかどうか。
     */
    SQCompiler(SQVM *v, SQLEXREADFUNC rg, SQUserPointer up, const SQChar* sourcename, bool raiseerror, bool lineinfo)
    {
        _vm=v;
        _lex.Init(_ss(v), rg, up,ThrowError,this);
        _sourcename = SQString::Create(_ss(v), sourcename);
        _lineinfo = lineinfo;_raiseerror = raiseerror;
        _scope.outers = 0;
        _scope.stacksize = 0;
        _compilererror[0] = _SC('\0');
    }
    /**
     * @brief 字句解析器からのエラーを中継する静的コールバック関数。
     * @details 字句解析器でエラーが発生した際に呼び出され、自身のErrorメンバ関数に処理を委譲します。
     * @param ud SQCompilerインスタンスへのポインタ。
     * @param s エラーメッセージ文字列。
     */
    static void ThrowError(void *ud, const SQChar *s) {
        SQCompiler *c = (SQCompiler *)ud;
        c->Error(s);
    }
    /**
     * @brief コンパイルエラーを報告し、ロングジャンプでコンパイル処理を中断する。
     * @details 書式指定文字列と可変長引数を受け取り、エラーメッセージを構築します。
     * その後、setjmpで設定されたエラーハンドリング箇所へジャンプします。
     * @param s 書式指定文字列。
     * @param ... 書式指定文字列に対応する可変長引数。
     */
    void Error(const SQChar *s, ...)
    {
        va_list vl;
        va_start(vl, s);
        scvsprintf(_compilererror, MAX_COMPILER_ERROR_LEN, s, vl);
        va_end(vl);
        longjmp(_errorjmp,1);
    }
    /**
     * @brief 字句解析器を呼び出し、次のトークンを取得する。
     * @details _lex.Lex()を呼び出し、結果を内部メンバ_tokenに格納します。
     */
    void Lex(){ _token = _lex.Lex();}
    /**
     * @brief 特定のトークンを期待し、消費する。
     * @details 現在のトークンが期待するトークンと一致するか検証します。
     * 一致しない場合はコンパイルエラーを発生させます。
     * 一致した場合、トークンの種類に応じてSQObjectを生成して返し、次のトークンに進みます。
     * @param tok 期待するトークンの種類。
     * @return トークンから生成されたSQObject。
     */
    SQObject Expect(SQInteger tok)
    {

        if(_token != tok) {
            if(_token == TK_CONSTRUCTOR && tok == TK_IDENTIFIER) {
                //do nothing
            }
            else {
                const SQChar *etypename;
                if(tok > 255) {
                    switch(tok)
                    {
                    case TK_IDENTIFIER:
                        etypename = _SC("IDENTIFIER");
                        break;
                    case TK_STRING_LITERAL:
                        etypename = _SC("STRING_LITERAL");
                        break;
                    case TK_INTEGER:
                        etypename = _SC("INTEGER");
                        break;
                    case TK_FLOAT:
                        etypename = _SC("FLOAT");
                        break;
                    default:
                        etypename = _lex.Tok2Str(tok);
                    }
                    Error(_SC("expected '%s'"), etypename);
                }
                Error(_SC("expected '%c'"), tok);
            }
        }
        SQObjectPtr ret;
        switch(tok)
        {
        case TK_IDENTIFIER:
            ret = _fs->CreateString(_lex._svalue);
            break;
        case TK_STRING_LITERAL:
            ret = _fs->CreateString(_lex._svalue,_lex._longstr.size()-1);
            break;
        case TK_INTEGER:
            ret = SQObjectPtr(_lex._nvalue);
            break;
        case TK_FLOAT:
            ret = SQObjectPtr(_lex._fvalue);
            break;
        }
        Lex();
        return ret;
    }
    /**
     * @brief 現在のトークンが文の終わりを示しているか判定する。
     * @details 直前のトークンが改行であるか、現在のトークンがスクリプトの終端(EOB)、'}'、';'のいずれかである場合にtrueを返します。
     * @return 文の終わりであればtrue、そうでなければfalse。
     */
    bool IsEndOfStatement() { return ((_lex._prevtoken == _SC('\n')) || (_token == SQUIRREL_EOB) || (_token == _SC('}')) || (_token == _SC(';'))); }
    /**
     * @brief 文末の任意で記述されるセミコロンを処理する。
     * @details 現在のトークンがセミコロンであればそれを消費します。
     * セミコロンでなく、かつ文の終わりでもない場合はエラーを発生させます。
     */
    void OptionalSemicolon()
    {
        if(_token == _SC(';')) { Lex(); return; }
        if(!IsEndOfStatement()) {
            Error(_SC("end of statement expected (; or lf)"));
        }
    }
    /**
     * @brief 現在のターゲットがローカル変数の場合にMOVE命令を生成する。
     * @details スタックトップのターゲットがローカル変数を指している場合、
     * その値を新しいターゲット位置に移動させるための_OP_MOVE命令を追加します。
     * これは、式の評価結果が一時的な位置ではなく、永続的なレジスタに格納されることを保証するために使用されます。
     */
    void MoveIfCurrentTargetIsLocal() {
        SQInteger trg = _fs->TopTarget();
        if(_fs->IsLocal(trg)) {
            trg = _fs->PopTarget(); //pops the target and moves it
            _fs->AddInstruction(_OP_MOVE, _fs->PushTarget(), trg);
        }
    }
    /**
     * @brief コンパイル処理のメインエントリポイント。
     * @details ソースコードを読み込み、解析し、最終的な関数プロトタイプオブジェクトを生成します。
     * @param[out] o コンパイル結果として生成された関数プロトタイプの格納先。
     * @return コンパイルが成功した場合はtrue、失敗した場合はfalse。
     */
    bool Compile(SQObjectPtr &o)
    {
        _debugline = 1;
        _debugop = 0;

        SQFuncState funcstate(_ss(_vm), NULL,ThrowError,this);
        funcstate._name = SQString::Create(_ss(_vm), _SC("main"));
        _fs = &funcstate;
        _fs->AddParameter(_fs->CreateString(_SC("this")));
        _fs->AddParameter(_fs->CreateString(_SC("vargv")));
        _fs->_varparams = true;
        _fs->_sourcename = _sourcename;
        SQInteger stacksize = _fs->GetStackSize();
        if(setjmp(_errorjmp) == 0) {
            Lex();
            while(_token > 0){
                Statement();
                if(_lex._prevtoken != _SC('}') && _lex._prevtoken != _SC(';')) OptionalSemicolon();
            }
            _fs->SetStackSize(stacksize);
            _fs->AddLineInfos(_lex._currentline, _lineinfo, true);
            _fs->AddInstruction(_OP_RETURN, 0xFF);
            _fs->SetStackSize(0);
            o =_fs->BuildProto();
#ifdef _DEBUG_DUMP
            _fs->Dump(_funcproto(o));
#endif
        }
        else {
            if(_raiseerror && _ss(_vm)->_compilererrorhandler) {
                _ss(_vm)->_compilererrorhandler(_vm, _compilererror, sq_type(_sourcename) == OT_STRING?_stringval(_sourcename):_SC("unknown"),
                    _lex._currentline, _lex._currentcolumn);
            }
            _vm->_lasterror = SQString::Create(_ss(_vm), _compilererror, -1);
            return false;
        }
        return true;
    }
    /**
     * @brief 一連の文を解析する。
     * @details '}'、TK_DEFAULT、TK_CASEトークンに遭遇するまで、文の解析を繰り返します。
     */
    void Statements()
    {
        while(_token != _SC('}') && _token != TK_DEFAULT && _token != TK_CASE) {
            Statement();
            if(_lex._prevtoken != _SC('}') && _lex._prevtoken != _SC(';')) OptionalSemicolon();
        }
    }
    /**
     * @brief 単一の文を解析し、対応するバイトコードを生成する。
     * @details 現在のトークンに応じて、if, while, for, returnなどの各種文の解析処理に分岐します。
     * @param closeframe 現在のスコープの終了時にフレームを閉じる(_OP_CLOSE)かどうか。
     */
    void Statement(bool closeframe = true)
    {
        _fs->AddLineInfos(_lex._currentline, _lineinfo);
        switch(_token){
        case _SC(';'):  Lex();                  break;
        case TK_IF:     IfStatement();          break;
        case TK_WHILE:      WhileStatement();       break;
        case TK_DO:     DoWhileStatement();     break;
        case TK_FOR:        ForStatement();         break;
        case TK_FOREACH:    ForEachStatement();     break;
        case TK_SWITCH: SwitchStatement();      break;
        case TK_LOCAL:      LocalDeclStatement();   break;
        case TK_RETURN:
        case TK_YIELD: {
            SQOpcode op;
            if(_token == TK_RETURN) {
                op = _OP_RETURN;
            }
            else {
                op = _OP_YIELD;
                _fs->_bgenerator = true;
            }
            Lex();
            if(!IsEndOfStatement()) {
                SQInteger retexp = _fs->GetCurrentPos()+1;
                CommaExpr();
                if(op == _OP_RETURN && _fs->_traps > 0)
                    _fs->AddInstruction(_OP_POPTRAP, _fs->_traps, 0);
                _fs->_returnexp = retexp;
                _fs->AddInstruction(op, 1, _fs->PopTarget(),_fs->GetStackSize());
            }
            else{
                if(op == _OP_RETURN && _fs->_traps > 0)
                    _fs->AddInstruction(_OP_POPTRAP, _fs->_traps ,0);
                _fs->_returnexp = -1;
                _fs->AddInstruction(op, 0xFF,0,_fs->GetStackSize());
            }
            break;}
        case TK_BREAK:
            if(_fs->_breaktargets.size() <= 0)Error(_SC("'break' has to be in a loop block"));
            if(_fs->_breaktargets.top() > 0){
                _fs->AddInstruction(_OP_POPTRAP, _fs->_breaktargets.top(), 0);
            }
            RESOLVE_OUTERS();
            _fs->AddInstruction(_OP_JMP, 0, -1234);
            _fs->_unresolvedbreaks.push_back(_fs->GetCurrentPos());
            Lex();
            break;
        case TK_CONTINUE:
            if(_fs->_continuetargets.size() <= 0)Error(_SC("'continue' has to be in a loop block"));
            if(_fs->_continuetargets.top() > 0) {
                _fs->AddInstruction(_OP_POPTRAP, _fs->_continuetargets.top(), 0);
            }
            RESOLVE_OUTERS();
            _fs->AddInstruction(_OP_JMP, 0, -1234);
            _fs->_unresolvedcontinues.push_back(_fs->GetCurrentPos());
            Lex();
            break;
        case TK_FUNCTION:
            FunctionStatement();
            break;
        case TK_CLASS:
            ClassStatement();
            break;
        case TK_ENUM:
            EnumStatement();
            break;
        case _SC('{'):{
                BEGIN_SCOPE();
                Lex();
                Statements();
                Expect(_SC('}'));
                if(closeframe) {
                    END_SCOPE();
                }
                else {
                    END_SCOPE_NO_CLOSE();
                }
            }
            break;
        case TK_TRY:
            TryCatchStatement();
            break;
        case TK_THROW:
            Lex();
            CommaExpr();
            _fs->AddInstruction(_OP_THROW, _fs->PopTarget());
            break;
        case TK_CONST:
            {
            Lex();
            SQObject id = Expect(TK_IDENTIFIER);
            Expect('=');
            SQObject val = ExpectScalar();
            OptionalSemicolon();
            SQTable *enums = _table(_ss(_vm)->_consts);
            SQObjectPtr strongid = id;
            enums->NewSlot(strongid,SQObjectPtr(val));
            strongid.Null();
            }
            break;
        default:
            CommaExpr();
            _fs->DiscardTarget();
            //_fs->PopTarget();
            break;
        }
        _fs->SnoozeOpt();
    }
    /**
     * @brief デリファレンス操作のオペコードを生成する。
     * @details スタックから値、キー、ソースオブジェクトを取得し、指定されたオペコード（_OP_NEWSLOT, _OP_SETなど）を持つ命令を追加します。
     * @param op 生成する命令のオペコード。
     */
    void EmitDerefOp(SQOpcode op)
    {
        SQInteger val = _fs->PopTarget();
        SQInteger key = _fs->PopTarget();
        SQInteger src = _fs->PopTarget();
        _fs->AddInstruction(op,_fs->PushTarget(),src,key,val);
    }
    /**
     * @brief 2つの引数を取るオペコードを生成する。
     * @details スタックから2つの引数（p1, p2）をポップし、指定されたオペコードを持つ命令を追加します。
     * @param op 生成する命令のオペコード。
     * @param p3 命令の第3引数（オプション）。
     */
    void Emit2ArgsOP(SQOpcode op, SQInteger p3 = 0)
    {
        SQInteger p2 = _fs->PopTarget(); //src in OP_GET
        SQInteger p1 = _fs->PopTarget(); //key in OP_GET
        _fs->AddInstruction(op,_fs->PushTarget(), p1, p2, p3);
    }
    /**
     * @brief 複合代入演算子(+=, -=など)のコードを生成する。
     * @details 左辺値の式タイプ（ローカル、オブジェクト、アウター）に応じて、適切な一連の命令を生成します。
     * @param tok 複合代入演算子のトークン。
     * @param etype 左辺値の式タイプ。
     * @param pos ローカルまたはアウター変数の場合の位置。
     */
    void EmitCompoundArith(SQInteger tok, SQInteger etype, SQInteger pos)
    {
        /* Generate code depending on the expression type */
        switch(etype) {
        case LOCAL:{
            SQInteger p2 = _fs->PopTarget(); //src in OP_GET
            SQInteger p1 = _fs->PopTarget(); //key in OP_GET
            _fs->PushTarget(p1);
            //EmitCompArithLocal(tok, p1, p1, p2);
            _fs->AddInstruction(ChooseArithOpByToken(tok),p1, p2, p1, 0);
            _fs->SnoozeOpt();
                   }
            break;
        case OBJECT:
        case BASE:
            {
                SQInteger val = _fs->PopTarget();
                SQInteger key = _fs->PopTarget();
                SQInteger src = _fs->PopTarget();
                /* _OP_COMPARITH mixes dest obj and source val in the arg1 */
                _fs->AddInstruction(_OP_COMPARITH, _fs->PushTarget(), (src<<16)|val, key, ChooseCompArithCharByToken(tok));
            }
            break;
        case OUTER:
            {
                SQInteger val = _fs->TopTarget();
                SQInteger tmp = _fs->PushTarget();
                _fs->AddInstruction(_OP_GETOUTER,   tmp, pos);
                _fs->AddInstruction(ChooseArithOpByToken(tok), tmp, val, tmp, 0);
                _fs->PopTarget();
                _fs->PopTarget();
                _fs->AddInstruction(_OP_SETOUTER, _fs->PushTarget(), pos, tmp);
            }
            break;
        }
    }
    /**
     * @brief カンマ区切りの式を解析する。
     * @details カンマが続く限り、式の解析を再帰的に行います。
     * 各式の評価後、結果は破棄されます（最後の式の結果のみが残る）。
     */
    void CommaExpr()
    {
        for(Expression();_token == ',';_fs->PopTarget(), Lex(), CommaExpr());
    }
    /**
     * @brief 式を解析します。
     * @details 代入演算子（'=', '+=', '-=' など）や三項演算子（'?:'）を含む、最も低い優先順位の式を解析します。
     * これは式解析の主要なエントリーポイントです。内部で `LogicalOrExp` を呼び出し、より高い優先順位の式の解析を開始します。
     */
    void Expression()
    {
         SQExpState es = _es;
        _es.etype     = EXPR;
        _es.epos      = -1;
        _es.donot_get = false;
        LogicalOrExp();
        switch(_token)  {
        case _SC('='):
        case TK_NEWSLOT:
        case TK_MINUSEQ:
        case TK_PLUSEQ:
        case TK_MULEQ:
        case TK_DIVEQ:
        case TK_MODEQ:{
            SQInteger op = _token;
            SQInteger ds = _es.etype;
            SQInteger pos = _es.epos;
            if(ds == EXPR) Error(_SC("can't assign expression"));
            else if(ds == BASE) Error(_SC("'base' cannot be modified"));
            Lex(); Expression();

            switch(op){
            case TK_NEWSLOT:
                if(ds == OBJECT || ds == BASE)
                    EmitDerefOp(_OP_NEWSLOT);
                else //if _derefstate != DEREF_NO_DEREF && DEREF_FIELD so is the index of a local
                    Error(_SC("can't 'create' a local slot"));
                break;
            case _SC('='): //ASSIGN
                switch(ds) {
                case LOCAL:
                    {
                        SQInteger src = _fs->PopTarget();
                        SQInteger dst = _fs->TopTarget();
                        _fs->AddInstruction(_OP_MOVE, dst, src);
                    }
                    break;
                case OBJECT:
                case BASE:
                    EmitDerefOp(_OP_SET);
                    break;
                case OUTER:
                    {
                        SQInteger src = _fs->PopTarget();
                        SQInteger dst = _fs->PushTarget();
                        _fs->AddInstruction(_OP_SETOUTER, dst, pos, src);
                    }
                }
                break;
            case TK_MINUSEQ:
            case TK_PLUSEQ:
            case TK_MULEQ:
            case TK_DIVEQ:
            case TK_MODEQ:
                EmitCompoundArith(op, ds, pos);
                break;
            }
            }
            break;
        case _SC('?'): {
            Lex();
            _fs->AddInstruction(_OP_JZ, _fs->PopTarget());
            SQInteger jzpos = _fs->GetCurrentPos();
            SQInteger trg = _fs->PushTarget();
            Expression();
            SQInteger first_exp = _fs->PopTarget();
            if(trg != first_exp) _fs->AddInstruction(_OP_MOVE, trg, first_exp);
            SQInteger endfirstexp = _fs->GetCurrentPos();
            _fs->AddInstruction(_OP_JMP, 0, 0);
            Expect(_SC(':'));
            SQInteger jmppos = _fs->GetCurrentPos();
            Expression();
            SQInteger second_exp = _fs->PopTarget();
            if(trg != second_exp) _fs->AddInstruction(_OP_MOVE, trg, second_exp);
            _fs->SetInstructionParam(jmppos, 1, _fs->GetCurrentPos() - jmppos);
            _fs->SetInstructionParam(jzpos, 1, endfirstexp - jzpos + 1);
            _fs->SnoozeOpt();
            }
            break;
        }
        _es = es;
    }
    /**
     * @brief 式のパーサー関数を呼び出すためのテンプレートヘルパー。
     * @details 式の状態(_es)を一時的に保存し、デフォルトの式状態（EXPR）にリセットしてから、引数で指定されたパーサー関数 f を実行します。
     * 実行後、元の式の状態を復元します。これにより、ネストした式の解析を安全に行うことができます。
     * @tparam T 呼び出すメンバ関数の型。
     * @param f 呼び出すコンパイラのメンバ関数ポインタ。
     */
    template<typename T> void INVOKE_EXP(T f)
    {
        SQExpState es = _es;
        _es.etype     = EXPR;
        _es.epos      = -1;
        _es.donot_get = false;
        (this->*f)();
        _es = es;
    }
    /**
     * @brief 二項演算子式を解析するためのテンプレートヘルパー。
     * @details 現在のトークンを消費し、より高い優先順位の式を解析するためにパーサー関数 `f` を呼び出します。
     * その後、スタックから2つのオペランドをポップし、指定されたオペコード `op` で二項演算命令を生成します。
     * @tparam T 呼び出すメンバ関数の型。
     * @param op 生成する命令のオペコード。
     * @param f 右辺を解析するために呼び出す、より高い優先順位のパーサー関数。
     * @param op3 命令に渡す追加の引数（ビット演算の種類など）。
     */
    template<typename T> void BIN_EXP(SQOpcode op, T f,SQInteger op3 = 0)
    {
        Lex();
        INVOKE_EXP(f);
        SQInteger op1 = _fs->PopTarget();SQInteger op2 = _fs->PopTarget();
        _fs->AddInstruction(op, _fs->PushTarget(), op1, op2, op3);
        _es.etype = EXPR;
    }
    /**
     * @brief 論理OR (||) 式を解析します。
     * @details 短絡評価を実装します。左辺式がtrueの場合、右辺式は評価されません。
     */
    void LogicalOrExp()
    {
        LogicalAndExp();
        for(;;) if(_token == TK_OR) {
            SQInteger first_exp = _fs->PopTarget();
            SQInteger trg = _fs->PushTarget();
            _fs->AddInstruction(_OP_OR, trg, 0, first_exp, 0);
            SQInteger jpos = _fs->GetCurrentPos();
            if(trg != first_exp) _fs->AddInstruction(_OP_MOVE, trg, first_exp);
            Lex(); INVOKE_EXP(&SQCompiler::LogicalOrExp);
            _fs->SnoozeOpt();
            SQInteger second_exp = _fs->PopTarget();
            if(trg != second_exp) _fs->AddInstruction(_OP_MOVE, trg, second_exp);
            _fs->SnoozeOpt();
            _fs->SetInstructionParam(jpos, 1, (_fs->GetCurrentPos() - jpos));
            _es.etype = EXPR;
            break;
        }else return;
    }
    /**
     * @brief 論理AND (&&) 式を解析します。
     * @details 短絡評価を実装します。左辺式がfalseの場合、右辺式は評価されません。
     */
    void LogicalAndExp()
    {
        BitwiseOrExp();
        for(;;) switch(_token) {
        case TK_AND: {
            SQInteger first_exp = _fs->PopTarget();
            SQInteger trg = _fs->PushTarget();
            _fs->AddInstruction(_OP_AND, trg, 0, first_exp, 0);
            SQInteger jpos = _fs->GetCurrentPos();
            if(trg != first_exp) _fs->AddInstruction(_OP_MOVE, trg, first_exp);
            Lex(); INVOKE_EXP(&SQCompiler::LogicalAndExp);
            _fs->SnoozeOpt();
            SQInteger second_exp = _fs->PopTarget();
            if(trg != second_exp) _fs->AddInstruction(_OP_MOVE, trg, second_exp);
            _fs->SnoozeOpt();
            _fs->SetInstructionParam(jpos, 1, (_fs->GetCurrentPos() - jpos));
            _es.etype = EXPR;
            break;
            }

        default:
            return;
        }
    }
    /**
     * @brief ビットごとのOR (|) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、ビットごとのOR演算命令を生成します。
     */
    void BitwiseOrExp()
    {
        BitwiseXorExp();
        for(;;) if(_token == _SC('|'))
        {BIN_EXP(_OP_BITW, &SQCompiler::BitwiseXorExp,BW_OR);
        }else return;
    }
    /**
     * @brief ビットごとのXOR (^) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、ビットごとのXOR演算命令を生成します。
     */
    void BitwiseXorExp()
    {
        BitwiseAndExp();
        for(;;) if(_token == _SC('^'))
        {BIN_EXP(_OP_BITW, &SQCompiler::BitwiseAndExp,BW_XOR);
        }else return;
    }
    /**
     * @brief ビットごとのAND (&) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、ビットごとのAND演算命令を生成します。
     */
    void BitwiseAndExp()
    {
        EqExp();
        for(;;) if(_token == _SC('&'))
        {BIN_EXP(_OP_BITW, &SQCompiler::EqExp,BW_AND);
        }else return;
    }
    /**
     * @brief 等価比較 (==, !=, <=>) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、等価比較の命令を生成します。
     */
    void EqExp()
    {
        CompExp();
        for(;;) switch(_token) {
        case TK_EQ: BIN_EXP(_OP_EQ, &SQCompiler::CompExp); break;
        case TK_NE: BIN_EXP(_OP_NE, &SQCompiler::CompExp); break;
        case TK_3WAYSCMP: BIN_EXP(_OP_CMP, &SQCompiler::CompExp,CMP_3W); break;
        default: return;
        }
    }
    /**
     * @brief 大小比較 (>, <, >=, <=, in, instanceof) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、大小比較や存在確認、インスタンス確認の命令を生成します。
     */
    void CompExp()
    {
        ShiftExp();
        for(;;) switch(_token) {
        case _SC('>'): BIN_EXP(_OP_CMP, &SQCompiler::ShiftExp,CMP_G); break;
        case _SC('<'): BIN_EXP(_OP_CMP, &SQCompiler::ShiftExp,CMP_L); break;
        case TK_GE: BIN_EXP(_OP_CMP, &SQCompiler::ShiftExp,CMP_GE); break;
        case TK_LE: BIN_EXP(_OP_CMP, &SQCompiler::ShiftExp,CMP_LE); break;
        case TK_IN: BIN_EXP(_OP_EXISTS, &SQCompiler::ShiftExp); break;
        case TK_INSTANCEOF: BIN_EXP(_OP_INSTANCEOF, &SQCompiler::ShiftExp); break;
        default: return;
        }
    }
    /**
     * @brief ビットシフト (<<, >>, >>>) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、ビットシフト演算命令を生成します。
     */
    void ShiftExp()
    {
        PlusExp();
        for(;;) switch(_token) {
        case TK_USHIFTR: BIN_EXP(_OP_BITW, &SQCompiler::PlusExp,BW_USHIFTR); break;
        case TK_SHIFTL: BIN_EXP(_OP_BITW, &SQCompiler::PlusExp,BW_SHIFTL); break;
        case TK_SHIFTR: BIN_EXP(_OP_BITW, &SQCompiler::PlusExp,BW_SHIFTR); break;
        default: return;
        }
    }
    /**
     * @brief トークンに基づいて算術演算のオペコードを選択します。
     * @details `+`, `-`, `*`, `/`, `%` や `+=`, `-=` などのトークンを、対応するVMのオペコード（`_OP_ADD`, `_OP_SUB`など）に変換します。
     * @param tok 変換するトークン。
     * @return 対応する算術演算オペコード。
     */
    SQOpcode ChooseArithOpByToken(SQInteger tok)
    {
        switch(tok) {
            case TK_PLUSEQ: case '+': return _OP_ADD;
            case TK_MINUSEQ: case '-': return _OP_SUB;
            case TK_MULEQ: case '*': return _OP_MUL;
            case TK_DIVEQ: case '/': return _OP_DIV;
            case TK_MODEQ: case '%': return _OP_MOD;
            default: assert(0);
        }
        return _OP_ADD;
    }
    /**
     * @brief 複合代入演算子のトークンに基づいて演算文字を選択します。
     * @details `+=`, `-=` などのトークンを、`_OP_COMPARITH` 命令で使用される演算子文字（`+`, `-`など）に変換します。
     * @param tok 変換する複合代入演算子のトークン。
     * @return 対応する演算子文字を表す整数。
     */
    SQInteger ChooseCompArithCharByToken(SQInteger tok)
    {
        SQInteger oper;
        switch(tok){
        case TK_MINUSEQ: oper = '-'; break;
        case TK_PLUSEQ: oper = '+'; break;
        case TK_MULEQ: oper = '*'; break;
        case TK_DIVEQ: oper = '/'; break;
        case TK_MODEQ: oper = '%'; break;
        default: oper = 0; //shut up compiler
            assert(0); break;
        };
        return oper;
    }
    /**
     * @brief 加算・減算 (+, -) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、加算・減算命令を生成します。
     */
    void PlusExp()
    {
        MultExp();
        for(;;) switch(_token) {
        case _SC('+'): case _SC('-'):
            BIN_EXP(ChooseArithOpByToken(_token), &SQCompiler::MultExp); break;
        default: return;
        }
    }

    /**
     * @brief 乗算・除算・剰余 (*, /, %) 式を解析します。
     * @details BIN_EXPヘルパーを利用して、乗算・除算・剰余命令を生成します。
     */
    void MultExp()
    {
        PrefixedExpr();
        for(;;) switch(_token) {
        case _SC('*'): case _SC('/'): case _SC('%'):
            BIN_EXP(ChooseArithOpByToken(_token), &SQCompiler::PrefixedExpr); break;
        default: return;
        }
    }
    /**
     * @brief 接頭辞式および後置式（メンバアクセス、関数呼び出し等）を解析します。
     * @details まず `Factor()` を呼び出して式の基本部分を解析し、その後ろに続くドット (`.`)、ブラケット (`[]`)、
     * 関数呼び出し (`()`)、後置インクリメント/デクリメント (`++`/`--`) などをループで処理します。
     */
    void PrefixedExpr()
    {
        SQInteger pos = Factor();
        for(;;) {
            switch(_token) {
            case _SC('.'):
                pos = -1;
                Lex();

                _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(Expect(TK_IDENTIFIER)));
                if(_es.etype==BASE) {
                    Emit2ArgsOP(_OP_GET);
                    pos = _fs->TopTarget();
                    _es.etype = EXPR;
                    _es.epos   = pos;
                }
                else {
                    if(NeedGet()) {
                        Emit2ArgsOP(_OP_GET);
                    }
                    _es.etype = OBJECT;
                }
                break;
            case _SC('['):
                if(_lex._prevtoken == _SC('\n')) Error(_SC("cannot break deref/or comma needed after [exp]=exp slot declaration"));
                Lex(); Expression(); Expect(_SC(']'));
                pos = -1;
                if(_es.etype==BASE) {
                    Emit2ArgsOP(_OP_GET);
                    pos = _fs->TopTarget();
                    _es.etype = EXPR;
                    _es.epos   = pos;
                }
                else {
                    if(NeedGet()) {
                        Emit2ArgsOP(_OP_GET);
                    }
                    _es.etype = OBJECT;
                }
                break;
            case TK_MINUSMINUS:
            case TK_PLUSPLUS:
                {
                    if(IsEndOfStatement()) return;
                    SQInteger diff = (_token==TK_MINUSMINUS) ? -1 : 1;
                    Lex();
                    switch(_es.etype)
                    {
                        case EXPR: Error(_SC("can't '++' or '--' an expression")); break;
                        case OBJECT:
                        case BASE:
                            if(_es.donot_get == true)  { Error(_SC("can't '++' or '--' an expression")); break; } //mmh dor this make sense?
                            Emit2ArgsOP(_OP_PINC, diff);
                            break;
                        case LOCAL: {
                            SQInteger src = _fs->PopTarget();
                            _fs->AddInstruction(_OP_PINCL, _fs->PushTarget(), src, 0, diff);
                                    }
                            break;
                        case OUTER: {
                            SQInteger tmp1 = _fs->PushTarget();
                            SQInteger tmp2 = _fs->PushTarget();
                            _fs->AddInstruction(_OP_GETOUTER, tmp2, _es.epos);
                            _fs->AddInstruction(_OP_PINCL,    tmp1, tmp2, 0, diff);
                            _fs->AddInstruction(_OP_SETOUTER, tmp2, _es.epos, tmp2);
                            _fs->PopTarget();
                        }
                    }
                }
                return;
                break;
            case _SC('('):
                switch(_es.etype) {
                    case OBJECT: {
                        SQInteger key     = _fs->PopTarget();  /* location of the key */
                        SQInteger table   = _fs->PopTarget();  /* location of the object */
                        SQInteger closure = _fs->PushTarget(); /* location for the closure */
                        SQInteger ttarget = _fs->PushTarget(); /* location for 'this' pointer */
                        _fs->AddInstruction(_OP_PREPCALL, closure, key, table, ttarget);
                        }
                        break;
                    case BASE:
                        //Emit2ArgsOP(_OP_GET);
                        _fs->AddInstruction(_OP_MOVE, _fs->PushTarget(), 0);
                        break;
                    case OUTER:
                        _fs->AddInstruction(_OP_GETOUTER, _fs->PushTarget(), _es.epos);
                        _fs->AddInstruction(_OP_MOVE,     _fs->PushTarget(), 0);
                        break;
                    default:
                        _fs->AddInstruction(_OP_MOVE, _fs->PushTarget(), 0);
                }
                _es.etype = EXPR;
                Lex();
                FunctionCallArgs();
                break;
            default: return;
            }
        }
    }
    /**
     * @brief 式の構成要素（因子）を解析します。
     * @details 演算子の優先順位の最下層に位置する要素を解析します。これには、数値や文字列リテラル、変数識別子、
     * `this`、`base`、`null`、テーブル/配列リテラル、括弧で囲まれた式、および単項演算子（`-`, `!`, `~`, `typeof`等）が含まれます。
     * @return 解析された変数がローカル変数の場合、そのスタック位置。それ以外の場合は-1。
     */
    SQInteger Factor()
    {
        //_es.etype = EXPR;
        switch(_token)
        {
        case TK_STRING_LITERAL:
            _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(_fs->CreateString(_lex._svalue,_lex._longstr.size()-1)));
            Lex();
            break;
        case TK_BASE:
            Lex();
            _fs->AddInstruction(_OP_GETBASE, _fs->PushTarget());
            _es.etype  = BASE;
            _es.epos   = _fs->TopTarget();
            return (_es.epos);
            break;
        case TK_IDENTIFIER:
        case TK_CONSTRUCTOR:
        case TK_THIS:{
                SQObject id;
                SQObject constant;

                switch(_token) {
                    case TK_IDENTIFIER:  id = _fs->CreateString(_lex._svalue);       break;
                    case TK_THIS:        id = _fs->CreateString(_SC("this"),4);        break;
                    case TK_CONSTRUCTOR: id = _fs->CreateString(_SC("constructor"),11); break;
                }

                SQInteger pos = -1;
                Lex();
                if((pos = _fs->GetLocalVariable(id)) != -1) {
                    /* Handle a local variable (includes 'this') */
                    _fs->PushTarget(pos);
                    _es.etype  = LOCAL;
                    _es.epos   = pos;
                }

                else if((pos = _fs->GetOuterVariable(id)) != -1) {
                    /* Handle a free var */
                    if(NeedGet()) {
                        _es.epos  = _fs->PushTarget();
                        _fs->AddInstruction(_OP_GETOUTER, _es.epos, pos);
                        /* _es.etype = EXPR; already default value */
                    }
                    else {
                        _es.etype = OUTER;
                        _es.epos  = pos;
                    }
                }

                else if(_fs->IsConstant(id, constant)) {
                    /* Handle named constant */
                    SQObjectPtr constval;
                    SQObject    constid;
                    if(sq_type(constant) == OT_TABLE) {
                        Expect('.');
                        constid = Expect(TK_IDENTIFIER);
                        if(!_table(constant)->Get(constid, constval)) {
                            constval.Null();
                            Error(_SC("invalid constant [%s.%s]"), _stringval(id), _stringval(constid));
                        }
                    }
                    else {
                        constval = constant;
                    }
                    _es.epos = _fs->PushTarget();

                    /* generate direct or literal function depending on size */
                    SQObjectType ctype = sq_type(constval);
                    switch(ctype) {
                        case OT_INTEGER: EmitLoadConstInt(_integer(constval),_es.epos); break;
                        case OT_FLOAT: EmitLoadConstFloat(_float(constval),_es.epos); break;
                        case OT_BOOL: _fs->AddInstruction(_OP_LOADBOOL, _es.epos, _integer(constval)); break;
                        default: _fs->AddInstruction(_OP_LOAD,_es.epos,_fs->GetConstant(constval)); break;
                    }
                    _es.etype = EXPR;
                }
                else {
                    /* Handle a non-local variable, aka a field. Push the 'this' pointer on
                    * the virtual stack (always found in offset 0, so no instruction needs to
                    * be generated), and push the key next. Generate an _OP_LOAD instruction
                    * for the latter. If we are not using the variable as a dref expr, generate
                    * the _OP_GET instruction.
                    */
                    _fs->PushTarget(0);
                    _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(id));
                    if(NeedGet()) {
                        Emit2ArgsOP(_OP_GET);
                    }
                    _es.etype = OBJECT;
                }
                return _es.epos;
            }
            break;
        case TK_DOUBLE_COLON:  // "::"
            _fs->AddInstruction(_OP_LOADROOT, _fs->PushTarget());
            _es.etype = OBJECT;
            _token = _SC('.'); /* hack: drop into PrefixExpr, case '.'*/
            _es.epos = -1;
            return _es.epos;
            break;
        case TK_NULL:
            _fs->AddInstruction(_OP_LOADNULLS, _fs->PushTarget(),1);
            Lex();
            break;
        case TK_INTEGER: EmitLoadConstInt(_lex._nvalue,-1); Lex();  break;
        case TK_FLOAT: EmitLoadConstFloat(_lex._fvalue,-1); Lex(); break;
        case TK_TRUE: case TK_FALSE:
            _fs->AddInstruction(_OP_LOADBOOL, _fs->PushTarget(),_token == TK_TRUE?1:0);
            Lex();
            break;
        case _SC('['): {
                _fs->AddInstruction(_OP_NEWOBJ, _fs->PushTarget(),0,0,NOT_ARRAY);
                SQInteger apos = _fs->GetCurrentPos(),key = 0;
                Lex();
                while(_token != _SC(']')) {
                    Expression();
                    if(_token == _SC(',')) Lex();
                    SQInteger val = _fs->PopTarget();
                    SQInteger array = _fs->TopTarget();
                    _fs->AddInstruction(_OP_APPENDARRAY, array, val, AAT_STACK);
                    key++;
                }
                _fs->SetInstructionParam(apos, 1, key);
                Lex();
            }
            break;
        case _SC('{'):
            _fs->AddInstruction(_OP_NEWOBJ, _fs->PushTarget(),0,NOT_TABLE);
            Lex();ParseTableOrClass(_SC(','),_SC('}'));
            break;
        case TK_FUNCTION: FunctionExp();break;
        case _SC('@'): FunctionExp(true);break;
        case TK_CLASS: Lex(); ClassExp();break;
        case _SC('-'):
            Lex();
            switch(_token) {
            case TK_INTEGER: EmitLoadConstInt(-_lex._nvalue,-1); Lex(); break;
            case TK_FLOAT: EmitLoadConstFloat(-_lex._fvalue,-1); Lex(); break;
            default: UnaryOP(_OP_NEG);
            }
            break;
        case _SC('!'): Lex(); UnaryOP(_OP_NOT); break;
        case _SC('~'):
            Lex();
            if(_token == TK_INTEGER)  { EmitLoadConstInt(~_lex._nvalue,-1); Lex(); break; }
            UnaryOP(_OP_BWNOT);
            break;
        case TK_TYPEOF : Lex() ;UnaryOP(_OP_TYPEOF); break;
        case TK_RESUME : Lex(); UnaryOP(_OP_RESUME); break;
        case TK_CLONE : Lex(); UnaryOP(_OP_CLONE); break;
        case TK_RAWCALL: Lex(); Expect('('); FunctionCallArgs(true); break;
        case TK_MINUSMINUS :
        case TK_PLUSPLUS :PrefixIncDec(_token); break;
        case TK_DELETE : DeleteExpr(); break;
        case _SC('('): Lex(); CommaExpr(); Expect(_SC(')'));
            break;
        case TK___LINE__: EmitLoadConstInt(_lex._currentline,-1); Lex(); break;
        case TK___FILE__: _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(_sourcename)); Lex(); break;
        default: Error(_SC("expression expected"));
        }
        _es.etype = EXPR;
        return -1;
    }
    /**
     * @brief 整数定数をロードする命令を生成します。
     * @details 値が32ビットに収まる場合は、より効率的な _OP_LOADINT 命令を使用します。
     * 収まらない場合は、定数テーブル経由で _OP_LOAD 命令を使用します。
     * @param value ロードする整数の値。
     * @param target 格納先のスタック位置。-1の場合、新しいターゲットをプッシュします。
     */
    void EmitLoadConstInt(SQInteger value,SQInteger target)
    {
        if(target < 0) {
            target = _fs->PushTarget();
        }
        if(value <= INT_MAX && value > INT_MIN) { //does it fit in 32 bits?
            _fs->AddInstruction(_OP_LOADINT, target,value);
        }
        else {
            _fs->AddInstruction(_OP_LOAD, target, _fs->GetNumericConstant(value));
        }
    }
    /**
     * @brief 浮動小数点数定数をロードする命令を生成します。
     * @details SQFloatが32ビットの場合は、_OP_LOADFLOAT 命令を使用します。
     * それ以外の場合は、定数テーブル経由で _OP_LOAD 命令を使用します。
     * @param value ロードする浮動小数点数の値。
     * @param target 格納先のスタック位置。-1の場合、新しいターゲットをプッシュします。
     */
    void EmitLoadConstFloat(SQFloat value,SQInteger target)
    {
        if(target < 0) {
            target = _fs->PushTarget();
        }
        if(sizeof(SQFloat) == sizeof(SQInt32)) {
            _fs->AddInstruction(_OP_LOADFLOAT, target,*((SQInt32 *)&value));
        }
        else {
            _fs->AddInstruction(_OP_LOAD, target, _fs->GetNumericConstant(value));
        }
    }
    /**
     * @brief 単項演算子の命令を生成します。
     * @details PrefixedExprを呼び出してオペランドを評価し、スタックにプッシュさせます。
     * その後、指定された単項演算オペコードを持つ命令を生成します。
     * @param op 生成する単項演算のオペコード。
     */
    void UnaryOP(SQOpcode op)
    {
        PrefixedExpr();
        SQInteger src = _fs->PopTarget();
        _fs->AddInstruction(op, _fs->PushTarget(), src);
    }
    /**
     * @brief オブジェクトのメンバアクセス時に値を取得(GET)する必要があるかを判断します。
     * @details 次のトークンが代入演算子や関数呼び出しなど、左辺値として扱われる文脈でない場合にGETが必要であると判断します。
     * これにより、`a.b = 1`のような代入文では不要なGET命令の生成を抑制します。
     * @return GET命令を生成する必要があればtrue、不要であればfalse。
     */
    bool NeedGet()
    {
        switch(_token) {
        case _SC('='): case _SC('('): case TK_NEWSLOT: case TK_MODEQ: case TK_MULEQ:
        case TK_DIVEQ: case TK_MINUSEQ: case TK_PLUSEQ:
            return false;
        case TK_PLUSPLUS: case TK_MINUSMINUS:
            if (!IsEndOfStatement()) {
                return false;
            }
        break;
        }
        return (!_es.donot_get || ( _es.donot_get && (_token == _SC('.') || _token == _SC('['))));
    }
    /**
     * @brief 関数呼び出しの引数リストを解析します。
     * @details `()`内のカンマ区切りの引数式を一つずつ評価し、_OP_CALL命令を生成します。
     * rawcallの場合は特別な引数処理を行います。また、`{}`が続く場合は、
     * 関数呼び出しの結果のテーブルに対してスロットを設定する構文を処理します。
     * @param rawcall `rawcall`キーワードによる呼び出しかどうかを示すフラグ。
     */
    void FunctionCallArgs(bool rawcall = false)
    {
        SQInteger nargs = 1;//this
         while(_token != _SC(')')) {
             Expression();
             MoveIfCurrentTargetIsLocal();
             nargs++;
             if(_token == _SC(',')){
                 Lex();
                 if(_token == ')') Error(_SC("expression expected, found ')'"));
             }
         }
         Lex();
         if (rawcall) {
             if (nargs < 3) Error(_SC("rawcall requires at least 2 parameters (callee and this)"));
             nargs -= 2; //removes callee and this from count
         }
         for(SQInteger i = 0; i < (nargs - 1); i++) _fs->PopTarget();
         SQInteger stackbase = _fs->PopTarget();
         SQInteger closure = _fs->PopTarget();
         _fs->AddInstruction(_OP_CALL, _fs->PushTarget(), closure, stackbase, nargs);
		 if (_token == '{')
		 {
			 SQInteger retval = _fs->TopTarget();
			 SQInteger nkeys = 0;
			 Lex();
			 while (_token != '}') {
				 switch (_token) {
				 case _SC('['):
					 Lex(); CommaExpr(); Expect(_SC(']'));
					 Expect(_SC('=')); Expression();
					 break;
				 default:
					 _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(Expect(TK_IDENTIFIER)));
					 Expect(_SC('=')); Expression();
					 break;
				 }
				 if (_token == ',') Lex();
				 nkeys++;
				 SQInteger val = _fs->PopTarget();
				 SQInteger key = _fs->PopTarget();
				 _fs->AddInstruction(_OP_SET, 0xFF, retval, key, val);
			 }
			 Lex();
		 }
    }
    /**
     * @brief テーブルまたはクラスの本体({}内)を解析します。
     * @details キーと値のペアを、指定されたterminatorトークンが現れるまで解析します。
     * separatorはエントリ間の区切り文字(テーブルは','、クラスは';')です。
     * クラスの場合、staticメンバや属性(<< >>)も処理します。
     * @param separator エントリ間の区切り文字。
     * @param terminator ブロックの終了を示すトークン。
     */
    void ParseTableOrClass(SQInteger separator,SQInteger terminator)
    {
        SQInteger tpos = _fs->GetCurrentPos(),nkeys = 0;
        while(_token != terminator) {
            bool hasattrs = false;
            bool isstatic = false;
            //check if is an attribute
            if(separator == ';') {
                if(_token == TK_ATTR_OPEN) {
                    _fs->AddInstruction(_OP_NEWOBJ, _fs->PushTarget(),0,NOT_TABLE); Lex();
                    ParseTableOrClass(',',TK_ATTR_CLOSE);
                    hasattrs = true;
                }
                if(_token == TK_STATIC) {
                    isstatic = true;
                    Lex();
                }
            }
            switch(_token) {
            case TK_FUNCTION:
            case TK_CONSTRUCTOR:{
                SQInteger tk = _token;
                Lex();
                SQObject id = tk == TK_FUNCTION ? Expect(TK_IDENTIFIER) : _fs->CreateString(_SC("constructor"));
				_fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(id));
				SQInteger boundtarget = 0xFF;
				if (_token == _SC('[')) {
					boundtarget = ParseBindEnv();
				}
                Expect(_SC('('));
                
                CreateFunction(id, boundtarget);
                _fs->AddInstruction(_OP_CLOSURE, _fs->PushTarget(), _fs->_functions.size() - 1, boundtarget);
                                }
                                break;
            case _SC('['):
                Lex(); CommaExpr(); Expect(_SC(']'));
                Expect(_SC('=')); Expression();
                break;
            case TK_STRING_LITERAL: //JSON
                if(separator == ',') { //only works for tables
                    _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(Expect(TK_STRING_LITERAL)));
                    Expect(_SC(':')); Expression();
                    break;
                }
            default :
                _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(Expect(TK_IDENTIFIER)));
                Expect(_SC('=')); Expression();
            }
            if(_token == separator) Lex();//optional comma/semicolon
            nkeys++;
            SQInteger val = _fs->PopTarget();
            SQInteger key = _fs->PopTarget();
            SQInteger attrs = hasattrs ? _fs->PopTarget():-1;
            ((void)attrs);
            assert((hasattrs && (attrs == key-1)) || !hasattrs);
            unsigned char flags = (hasattrs?NEW_SLOT_ATTRIBUTES_FLAG:0)|(isstatic?NEW_SLOT_STATIC_FLAG:0);
            SQInteger table = _fs->TopTarget(); //<<BECAUSE OF THIS NO COMMON EMIT FUNC IS POSSIBLE
            if(separator == _SC(',')) { //hack recognizes a table from the separator
                _fs->AddInstruction(_OP_NEWSLOT, 0xFF, table, key, val);
            }
            else {
                _fs->AddInstruction(_OP_NEWSLOTA, flags, table, key, val); //this for classes only as it invokes _newmember
            }
        }
        if(separator == _SC(',')) //hack recognizes a table from the separator
            _fs->SetInstructionParam(tpos, 1, nkeys);
        Lex();
    }
    /**
     * @brief ローカル変数またはローカル関数の宣言文を解析します。
     * @details `local`キーワードに続く識別子をローカル変数として登録します。初期化式がある場合はそれを評価します。
     * カンマで複数の変数を一度に宣言することも可能です。`local function ...`構文もここで処理されます。
     */
    void LocalDeclStatement()
    {
        SQObject varname;
        Lex();
        if( _token == TK_FUNCTION) {
			SQInteger boundtarget = 0xFF;
            Lex();
			varname = Expect(TK_IDENTIFIER);
			if (_token == _SC('[')) {
				boundtarget = ParseBindEnv();
			}
            Expect(_SC('('));
            CreateFunction(varname,0xFF,false);
            _fs->AddInstruction(_OP_CLOSURE, _fs->PushTarget(), _fs->_functions.size() - 1, boundtarget);
            _fs->PopTarget();
            _fs->PushLocalVariable(varname);
            return;
        }

        do {
            varname = Expect(TK_IDENTIFIER);
            if(_token == _SC('=')) {
                Lex(); Expression();
                SQInteger src = _fs->PopTarget();
                SQInteger dest = _fs->PushTarget();
                if(dest != src) _fs->AddInstruction(_OP_MOVE, dest, src);
            }
            else{
                _fs->AddInstruction(_OP_LOADNULLS, _fs->PushTarget(),1);
            }
            _fs->PopTarget();
            _fs->PushLocalVariable(varname);
            if(_token == _SC(',')) Lex(); else break;
        } while(1);
    }
    /**
     * @brief `if`文または`else`文の本体ブロックを解析します。
     * @details ブロックが`{}`で囲まれているか、単一の文であるかを判別し、適切に解析処理を呼び出します。
     */
    void IfBlock()
    {
        if (_token == _SC('{'))
        {
            BEGIN_SCOPE();
            Lex();
            Statements();
            Expect(_SC('}'));
            if (true) {
                END_SCOPE();
            }
            else {
                END_SCOPE_NO_CLOSE();
            }
        }
        else {
            //BEGIN_SCOPE();
            Statement();
            if (_lex._prevtoken != _SC('}') && _lex._prevtoken != _SC(';')) OptionalSemicolon();
            //END_SCOPE();
        }
    }
    /**
     * @brief `if-else`文を解析します。
     * @details 条件式を評価し、_OP_JZ (Jump if Zero) 命令を生成して条件が偽の場合のジャンプを設定します。
     * `if`ブロックと、存在すれば`else`ブロックを解析し、ジャンプ先アドレスを解決します。
     */
    void IfStatement()
    {
        SQInteger jmppos;
        bool haselse = false;
        Lex(); Expect(_SC('(')); CommaExpr(); Expect(_SC(')'));
        _fs->AddInstruction(_OP_JZ, _fs->PopTarget());
        SQInteger jnepos = _fs->GetCurrentPos();



        IfBlock();
        //
        /*static int n = 0;
        if (_token != _SC('}') && _token != TK_ELSE) {
            printf("IF %d-----------------------!!!!!!!!!\n", n);
            if (n == 5)
            {
                printf("asd");
            }
            n++;
            //OptionalSemicolon();
        }*/


        SQInteger endifblock = _fs->GetCurrentPos();
        if(_token == TK_ELSE){
            haselse = true;
            //BEGIN_SCOPE();
            _fs->AddInstruction(_OP_JMP);
            jmppos = _fs->GetCurrentPos();
            Lex();
            //Statement(); if(_lex._prevtoken != _SC('}')) OptionalSemicolon();
            IfBlock();
            //END_SCOPE();
            _fs->SetInstructionParam(jmppos, 1, _fs->GetCurrentPos() - jmppos);
        }
        _fs->SetInstructionParam(jnepos, 1, endifblock - jnepos + (haselse?1:0));
    }
    /**
     * @brief `while`ループを解析します。
     * @details ループ開始前に条件を評価し(_OP_JZ)、ループ本体の後にループ先頭への無条件ジャンプ(_OP_JMP)を生成します。
     * `break`/`continue`にも対応します。
     */
    void WhileStatement()
    {
        SQInteger jzpos, jmppos;
        jmppos = _fs->GetCurrentPos();
        Lex(); Expect(_SC('(')); CommaExpr(); Expect(_SC(')'));

        BEGIN_BREAKBLE_BLOCK();
        _fs->AddInstruction(_OP_JZ, _fs->PopTarget());
        jzpos = _fs->GetCurrentPos();
        BEGIN_SCOPE();

        Statement();

        END_SCOPE();
        _fs->AddInstruction(_OP_JMP, 0, jmppos - _fs->GetCurrentPos() - 1);
        _fs->SetInstructionParam(jzpos, 1, _fs->GetCurrentPos() - jzpos);

        END_BREAKBLE_BLOCK(jmppos);
    }
    /**
     * @brief `do-while`ループを解析します。
     * @details 最初にループ本体を実行し、その後に条件を評価してループの継続を判断します。`break`/`continue`にも対応します。
     */
    void DoWhileStatement()
    {
        Lex();
        SQInteger jmptrg = _fs->GetCurrentPos();
        BEGIN_BREAKBLE_BLOCK()
        BEGIN_SCOPE();
        Statement();
        END_SCOPE();
        Expect(TK_WHILE);
        SQInteger continuetrg = _fs->GetCurrentPos();
        Expect(_SC('(')); CommaExpr(); Expect(_SC(')'));
        _fs->AddInstruction(_OP_JZ, _fs->PopTarget(), 1);
        _fs->AddInstruction(_OP_JMP, 0, jmptrg - _fs->GetCurrentPos() - 1);
        END_BREAKBLE_BLOCK(continuetrg);
    }
    /**
     * @brief Cスタイルの`for`ループ (`for(init; cond; inc)`)を解析します。
     * @details 初期化、条件、更新の各部分を解析し、ループ構造をJMP命令とJZ命令で構築します。
     * 更新部分はループ本体の実行後に評価されるよう、命令列を一時的に退避・再配置します。
     */
    void ForStatement()
    {
        Lex();
        BEGIN_SCOPE();
        Expect(_SC('('));
        if(_token == TK_LOCAL) LocalDeclStatement();
        else if(_token != _SC(';')){
            CommaExpr();
            _fs->PopTarget();
        }
        Expect(_SC(';'));
        _fs->SnoozeOpt();
        SQInteger jmppos = _fs->GetCurrentPos();
        SQInteger jzpos = -1;
        if(_token != _SC(';')) { CommaExpr(); _fs->AddInstruction(_OP_JZ, _fs->PopTarget()); jzpos = _fs->GetCurrentPos(); }
        Expect(_SC(';'));
        _fs->SnoozeOpt();
        SQInteger expstart = _fs->GetCurrentPos() + 1;
        if(_token != _SC(')')) {
            CommaExpr();
            _fs->PopTarget();
        }
        Expect(_SC(')'));
        _fs->SnoozeOpt();
        SQInteger expend = _fs->GetCurrentPos();
        SQInteger expsize = (expend - expstart) + 1;
        SQInstructionVec exp;
        if(expsize > 0) {
            for(SQInteger i = 0; i < expsize; i++)
                exp.push_back(_fs->GetInstruction(expstart + i));
            _fs->PopInstructions(expsize);
        }
        BEGIN_BREAKBLE_BLOCK()
        Statement();
        SQInteger continuetrg = _fs->GetCurrentPos();
        if(expsize > 0) {
            for(SQInteger i = 0; i < expsize; i++)
                _fs->AddInstruction(exp[i]);
        }
        _fs->AddInstruction(_OP_JMP, 0, jmppos - _fs->GetCurrentPos() - 1, 0);
        if(jzpos>  0) _fs->SetInstructionParam(jzpos, 1, _fs->GetCurrentPos() - jzpos);
        
        END_BREAKBLE_BLOCK(continuetrg);

		END_SCOPE();
    }
    /**
     * @brief `foreach`ループ (`foreach(val, key in container)`)を解析します。
     * @details イテレータ用の内部ローカル変数を確保し、_OP_FOREACH命令と_OP_POSTFOREACH命令を使ってイテレーションを実装します。
     */
    void ForEachStatement()
    {
        SQObject idxname, valname;
        Lex(); Expect(_SC('(')); valname = Expect(TK_IDENTIFIER);
        if(_token == _SC(',')) {
            idxname = valname;
            Lex(); valname = Expect(TK_IDENTIFIER);
        }
        else{
            idxname = _fs->CreateString(_SC("@INDEX@"));
        }
        Expect(TK_IN);

        //save the stack size
        BEGIN_SCOPE();
        //put the table in the stack(evaluate the table expression)
        Expression(); Expect(_SC(')'));
        SQInteger container = _fs->TopTarget();
        //push the index local var
        SQInteger indexpos = _fs->PushLocalVariable(idxname);
        _fs->AddInstruction(_OP_LOADNULLS, indexpos,1);
        //push the value local var
        SQInteger valuepos = _fs->PushLocalVariable(valname);
        _fs->AddInstruction(_OP_LOADNULLS, valuepos,1);
        //push reference index
        SQInteger itrpos = _fs->PushLocalVariable(_fs->CreateString(_SC("@ITERATOR@"))); //use invalid id to make it inaccessible
        _fs->AddInstruction(_OP_LOADNULLS, itrpos,1);
        SQInteger jmppos = _fs->GetCurrentPos();
        _fs->AddInstruction(_OP_FOREACH, container, 0, indexpos);
        SQInteger foreachpos = _fs->GetCurrentPos();
        _fs->AddInstruction(_OP_POSTFOREACH, container, 0, indexpos);
        //generate the statement code
        BEGIN_BREAKBLE_BLOCK()
        Statement();
        _fs->AddInstruction(_OP_JMP, 0, jmppos - _fs->GetCurrentPos() - 1);
        _fs->SetInstructionParam(foreachpos, 1, _fs->GetCurrentPos() - foreachpos);
        _fs->SetInstructionParam(foreachpos + 1, 1, _fs->GetCurrentPos() - foreachpos);
        END_BREAKBLE_BLOCK(foreachpos - 1);
        //restore the local variable stack(remove index,val and ref idx)
        _fs->PopTarget();
        END_SCOPE();
    }
    /**
     * @brief `switch`文を解析します。
     * @details 各`case`節で_OP_EQを使って式と値を比較し、_OP_JZで次の`case`へジャンプするコードを連鎖的に生成します。
     * `default`節と`break`文の解決も行います。
     */
    void SwitchStatement()
    {
        Lex(); Expect(_SC('(')); CommaExpr(); Expect(_SC(')'));
        Expect(_SC('{'));
        SQInteger expr = _fs->TopTarget();
        bool bfirst = true;
        SQInteger tonextcondjmp = -1;
        SQInteger skipcondjmp = -1;
        SQInteger __nbreaks__ = _fs->_unresolvedbreaks.size();
        _fs->_breaktargets.push_back(0);
        while(_token == TK_CASE) {
            if(!bfirst) {
                _fs->AddInstruction(_OP_JMP, 0, 0);
                skipcondjmp = _fs->GetCurrentPos();
                _fs->SetInstructionParam(tonextcondjmp, 1, _fs->GetCurrentPos() - tonextcondjmp);
            }
            //condition
            Lex(); Expression(); Expect(_SC(':'));
            SQInteger trg = _fs->PopTarget();
            SQInteger eqtarget = trg;
            bool local = _fs->IsLocal(trg);
            if(local) {
                eqtarget = _fs->PushTarget(); //we need to allocate a extra reg
            }
            _fs->AddInstruction(_OP_EQ, eqtarget, trg, expr);
            _fs->AddInstruction(_OP_JZ, eqtarget, 0);
            if(local) {
                _fs->PopTarget();
            }

            //end condition
            if(skipcondjmp != -1) {
                _fs->SetInstructionParam(skipcondjmp, 1, (_fs->GetCurrentPos() - skipcondjmp));
            }
            tonextcondjmp = _fs->GetCurrentPos();
            BEGIN_SCOPE();
            Statements();
            END_SCOPE();
            bfirst = false;
        }
        if(tonextcondjmp != -1)
            _fs->SetInstructionParam(tonextcondjmp, 1, _fs->GetCurrentPos() - tonextcondjmp);
        if(_token == TK_DEFAULT) {
            Lex(); Expect(_SC(':'));
            BEGIN_SCOPE();
            Statements();
            END_SCOPE();
        }
        Expect(_SC('}'));
        _fs->PopTarget();
        __nbreaks__ = _fs->_unresolvedbreaks.size() - __nbreaks__;
        if(__nbreaks__ > 0)ResolveBreaks(_fs, __nbreaks__);
        _fs->_breaktargets.pop_back();
    }
    /**
     * @brief `function`文による関数宣言を解析します。
     * @details `function MyFunc() ...` や `function MyTable.MyFunc() ...` といった構文を解析し、
     * 関数クロージャを生成して、指定されたスロットに新しいスロットとして設定します。
     */
    void FunctionStatement()
    {
        SQObject id;
        Lex(); id = Expect(TK_IDENTIFIER);
        _fs->PushTarget(0);
        _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(id));
        if(_token == TK_DOUBLE_COLON) Emit2ArgsOP(_OP_GET);

        while(_token == TK_DOUBLE_COLON) {
            Lex();
            id = Expect(TK_IDENTIFIER);
            _fs->AddInstruction(_OP_LOAD, _fs->PushTarget(), _fs->GetConstant(id));
            if(_token == TK_DOUBLE_COLON) Emit2ArgsOP(_OP_GET);
        }
		SQInteger boundtarget = 0xFF;
		if (_token == _SC('[')) {
			boundtarget = ParseBindEnv();
		}
        Expect(_SC('('));
        CreateFunction(id, boundtarget);
        _fs->AddInstruction(_OP_CLOSURE, _fs->PushTarget(), _fs->_functions.size() - 1, boundtarget);
        EmitDerefOp(_OP_NEWSLOT);
        _fs->PopTarget();
    }
    /**
     * @brief `class`文によるクラス宣言を解析します。
     * @details `class MyClass ...` や `class MyNamespace.MyClass ...` といった構文を解析します。
     * クラス式を評価し、結果を指定されたスロットに新しいスロットとして設定します。
     */
    void ClassStatement()
    {
        SQExpState es;
        Lex();
        es = _es;
        _es.donot_get = true;
        PrefixedExpr();
        if(_es.etype == EXPR) {
            Error(_SC("invalid class name"));
        }
        else if(_es.etype == OBJECT || _es.etype == BASE) {
            ClassExp();
            EmitDerefOp(_OP_NEWSLOT);
            _fs->PopTarget();
        }
        else {
            Error(_SC("cannot create a class in a local with the syntax(class <local>)"));
        }
        _es = es;
    }
    /**
     * @brief スカラー値(整数、浮動小数点数、文字列、真偽値)を期待し、解析します。
     * @details `const`宣言や`enum`宣言の右辺値など、コンパイル時に値が確定するスカラー値を解析するために使用されます。
     * @return 解析されたスカラー値を格納したSQObject。
     */
    SQObject ExpectScalar()
    {
        SQObject val;
        val._type = OT_NULL; val._unVal.nInteger = 0; //shut up GCC 4.x
        switch(_token) {
            case TK_INTEGER:
                val._type = OT_INTEGER;
                val._unVal.nInteger = _lex._nvalue;
                break;
            case TK_FLOAT:
                val._type = OT_FLOAT;
                val._unVal.fFloat = _lex._fvalue;
                break;
            case TK_STRING_LITERAL:
                val = _fs->CreateString(_lex._svalue,_lex._longstr.size()-1);
                break;
            case TK_TRUE:
            case TK_FALSE:
                val._type = OT_BOOL;
                val._unVal.nInteger = _token == TK_TRUE ? 1 : 0;
                break;
            case '-':
                Lex();
                switch(_token)
                {
                case TK_INTEGER:
                    val._type = OT_INTEGER;
                    val._unVal.nInteger = -_lex._nvalue;
                break;
                case TK_FLOAT:
                    val._type = OT_FLOAT;
                    val._unVal.fFloat = -_lex._fvalue;
                break;
                default:
                    Error(_SC("scalar expected : integer, float"));
                }
                break;
            default:
                Error(_SC("scalar expected : integer, float, or string"));
        }
        Lex();
        return val;
    }
    /**
     * @brief `enum` (列挙型) の宣言を解析します。
     * @details `enum`ブロック内の識別子と値を解析し、定数テーブルに新しいテーブルとして登録します。
     * 値が指定されない場合は、0から始まる連番が自動的に割り当てられます。
     */
    void EnumStatement()
    {
        Lex();
        SQObject id = Expect(TK_IDENTIFIER);
        Expect(_SC('{'));

        SQObject table = _fs->CreateTable();
        SQInteger nval = 0;
        while(_token != _SC('}')) {
            SQObject key = Expect(TK_IDENTIFIER);
            SQObject val;
            if(_token == _SC('=')) {
                Lex();
                val = ExpectScalar();
            }
            else {
                val._type = OT_INTEGER;
                val._unVal.nInteger = nval++;
            }
            _table(table)->NewSlot(SQObjectPtr(key),SQObjectPtr(val));
            if(_token == ',') Lex();
        }
        SQTable *enums = _table(_ss(_vm)->_consts);
        SQObjectPtr strongid = id;
        enums->NewSlot(SQObjectPtr(strongid),SQObjectPtr(table));
        strongid.Null();
        Lex();
    }
    /**
     * @brief `try-catch`文を解析します。
     * @details _OP_PUSHTRAP命令で例外ハンドラを設定し、`try`ブロックを解析します。
     * `catch`ブロックでは、捕捉した例外をローカル変数に束縛し、ブロック内の文を解析します。
     */
    void TryCatchStatement()
    {
        SQObject exid;
        Lex();
        _fs->AddInstruction(_OP_PUSHTRAP,0,0);
        _fs->_traps++;
        if(_fs->_breaktargets.size()) _fs->_breaktargets.top()++;
        if(_fs->_continuetargets.size()) _fs->_continuetargets.top()++;
        SQInteger trappos = _fs->GetCurrentPos();
        {
            BEGIN_SCOPE();
            Statement();
            END_SCOPE();
        }
        _fs->_traps--;
        _fs->AddInstruction(_OP_POPTRAP, 1, 0);
        if(_fs->_breaktargets.size()) _fs->_breaktargets.top()--;
        if(_fs->_continuetargets.size()) _fs->_continuetargets.top()--;
        _fs->AddInstruction(_OP_JMP, 0, 0);
        SQInteger jmppos = _fs->GetCurrentPos();
        _fs->SetInstructionParam(trappos, 1, (_fs->GetCurrentPos() - trappos));
        Expect(TK_CATCH); Expect(_SC('(')); exid = Expect(TK_IDENTIFIER); Expect(_SC(')'));
        {
            BEGIN_SCOPE();
            SQInteger ex_target = _fs->PushLocalVariable(exid);
            _fs->SetInstructionParam(trappos, 0, ex_target);
            Statement();
            _fs->SetInstructionParams(jmppos, 0, (_fs->GetCurrentPos() - jmppos), 0);
            END_SCOPE();
        }
    }
	/**
	 * @brief 関数の環境束縛構文 `[...]` を解析します。
	 * @details `function foo[...](){}` のような構文で、`[]`内の式を評価し、
	 * その結果を関数の環境として束縛するための準備をします。
	 * @return 束縛される環境オブジェクトが格納されているスタック上の位置。
	 */
	SQInteger ParseBindEnv()
	{
		SQInteger boundtarget;
		Lex();
		Expression();
		boundtarget = _fs->TopTarget();
		Expect(_SC(']'));
		return boundtarget;
	}
    /**
     * @brief 関数式 (`function(){...}`) またはラムダ式 (`@(){...}`) を解析します。
     * @details 無名関数またはラムダ式を解析し、クロージャを生成してスタックにプッシュします。
     * @param lambda ラムダ式 (`@`) かどうかを示すフラグ。
     */
    void FunctionExp(bool lambda = false)
    {
        Lex(); 
		SQInteger boundtarget = 0xFF;
		if (_token == _SC('[')) {
			boundtarget = ParseBindEnv();
		}
		Expect(_SC('('));
        SQObjectPtr dummy;
        CreateFunction(dummy, boundtarget, lambda);
        _fs->AddInstruction(_OP_CLOSURE, _fs->PushTarget(), _fs->_functions.size() - 1, boundtarget);
    }
    /**
     * @brief クラス式 (`class extends ... {}`) を解析します。
     * @details `extends`による継承や、`<<...>>`による属性の指定を処理し、
     * 新しいクラスオブジェクトを生成するための_OP_NEWOBJ命令を追加します。
     * その後、クラス本体を解析します。
     */
    void ClassExp()
    {
        SQInteger base = -1;
        SQInteger attrs = -1;
        if(_token == TK_EXTENDS) {
            Lex(); Expression();
            base = _fs->TopTarget();
        }
        if(_token == TK_ATTR_OPEN) {
            Lex();
            _fs->AddInstruction(_OP_NEWOBJ, _fs->PushTarget(),0,NOT_TABLE);
            ParseTableOrClass(_SC(','),TK_ATTR_CLOSE);
            attrs = _fs->TopTarget();
        }
        Expect(_SC('{'));
        if(attrs != -1) _fs->PopTarget();
        if(base != -1) _fs->PopTarget();
        _fs->AddInstruction(_OP_NEWOBJ, _fs->PushTarget(), base, attrs,NOT_CLASS);
        ParseTableOrClass(_SC(';'),_SC('}'));
    }
    /**
     * @brief `delete`式を解析します。
     * @details `delete table.slot` のような構文を解析し、_OP_DELETE命令を生成します。
     * ローカル変数や式自体は削除できないため、エラーチェックも行います。
     */
    void DeleteExpr()
    {
        SQExpState es;
        Lex();
        es = _es;
        _es.donot_get = true;
        PrefixedExpr();
        if(_es.etype==EXPR) Error(_SC("can't delete an expression"));
        if(_es.etype==OBJECT || _es.etype==BASE) {
            Emit2ArgsOP(_OP_DELETE);
        }
        else {
            Error(_SC("cannot delete an (outer) local"));
        }
        _es = es;
    }
    /**
     * @brief 前置インクリメント/デクリメント (`++var`, `--var`) を解析します。
     * @details 式の種類(ローカル、オブジェクト、アウター)に応じて、適切なインクリメント/デクリメント命令を生成します。
     * @param token TK_PLUSPLUS または TK_MINUSMINUS。
     */
    void PrefixIncDec(SQInteger token)
    {
        SQExpState  es;
        SQInteger diff = (token==TK_MINUSMINUS) ? -1 : 1;
        Lex();
        es = _es;
        _es.donot_get = true;
        PrefixedExpr();
        if(_es.etype==EXPR) {
            Error(_SC("can't '++' or '--' an expression"));
        }
        else if(_es.etype==OBJECT || _es.etype==BASE) {
            Emit2ArgsOP(_OP_INC, diff);
        }
        else if(_es.etype==LOCAL) {
            SQInteger src = _fs->TopTarget();
            _fs->AddInstruction(_OP_INCL, src, src, 0, diff);

        }
        else if(_es.etype==OUTER) {
            SQInteger tmp = _fs->PushTarget();
            _fs->AddInstruction(_OP_GETOUTER, tmp, _es.epos);
            _fs->AddInstruction(_OP_INCL,     tmp, tmp, 0, diff);
            _fs->AddInstruction(_OP_SETOUTER, tmp, _es.epos, tmp);
        }
        _es = es;
    }
    /**
     * @brief 関数プロトタイプを生成します。
     * @details 新しい関数状態(SQFuncState)を作成し、仮引数、デフォルト引数、可変長引数を解析します。
     * その後、関数本体を解析し、最終的にSQFunctionProtoオブジェクトを構築して親の関数状態に登録します。
     * @param name 関数の名前。
     * @param boundtarget 環境束縛のターゲットとなるスタック位置。束縛がない場合は0xFF。
     * @param lambda ラムダ式かどうかを示すフラグ。
     */
    void CreateFunction(SQObject &name,SQInteger boundtarget,bool lambda = false)
    {
        SQFuncState *funcstate = _fs->PushChildState(_ss(_vm));
        funcstate->_name = name;
        SQObject paramname;
        funcstate->AddParameter(_fs->CreateString(_SC("this")));
        funcstate->_sourcename = _sourcename;
        SQInteger defparams = 0;
        while(_token!=_SC(')')) {
            if(_token == TK_VARPARAMS) {
                if(defparams > 0) Error(_SC("function with default parameters cannot have variable number of parameters"));
                funcstate->AddParameter(_fs->CreateString(_SC("vargv")));
                funcstate->_varparams = true;
                Lex();
                if(_token != _SC(')')) Error(_SC("expected ')'"));
                break;
            }
            else {
                paramname = Expect(TK_IDENTIFIER);
                funcstate->AddParameter(paramname);
                if(_token == _SC('=')) {
                    Lex();
                    Expression();
                    funcstate->AddDefaultParam(_fs->TopTarget());
                    defparams++;
                }
                else {
                    if(defparams > 0) Error(_SC("expected '='"));
                }
                if(_token == _SC(',')) Lex();
                else if(_token != _SC(')')) Error(_SC("expected ')' or ','"));
            }
        }
        Expect(_SC(')'));
		if (boundtarget != 0xFF) {
			_fs->PopTarget();
		}
        for(SQInteger n = 0; n < defparams; n++) {
            _fs->PopTarget();
        }

        SQFuncState *currchunk = _fs;
        _fs = funcstate;
        if(lambda) {
            Expression();
            _fs->AddInstruction(_OP_RETURN, 1, _fs->PopTarget());}
        else {
            Statement(false);
        }
        funcstate->AddLineInfos(_lex._prevtoken == _SC('\n')?_lex._lasttokenline:_lex._currentline, _lineinfo, true);
        funcstate->AddInstruction(_OP_RETURN, -1);
        funcstate->SetStackSize(0);

        SQFunctionProto *func = funcstate->BuildProto();
#ifdef _DEBUG_DUMP
        funcstate->Dump(func);
#endif
        _fs = currchunk;
        _fs->_functions.push_back(func);
        _fs->PopChildState();
    }
    /**
     * @brief 未解決の`break`文のジャンプ先を解決します。
     * @details ループやswitchブロックの終端で呼び出され、未解決のbreakリストを逆順にたどり、
     * JMP命令の引数に現在の命令位置を設定します。
     * @param funcstate 現在の関数状態。
     * @param ntoresolve 解決する`break`の数。
     */
    void ResolveBreaks(SQFuncState *funcstate, SQInteger ntoresolve)
    {
        while(ntoresolve > 0) {
            SQInteger pos = funcstate->_unresolvedbreaks.back();
            funcstate->_unresolvedbreaks.pop_back();
            //set the jmp instruction
            funcstate->SetInstructionParams(pos, 0, funcstate->GetCurrentPos() - pos, 0);
            ntoresolve--;
        }
    }
    /**
     * @brief 未解決の`continue`文のジャンプ先を解決します。
     * @details ループの終端で呼び出され、未解決のcontinueリストを逆順にたどり、
     * JMP命令の引数にループの先頭(または更新部)の位置を設定します。
     * @param funcstate 現在の関数状態。
     * @param ntoresolve 解決する`continue`の数。
     * @param targetpos ジャンプ先の命令位置。
     */
    void ResolveContinues(SQFuncState *funcstate, SQInteger ntoresolve, SQInteger targetpos)
    {
        while(ntoresolve > 0) {
            SQInteger pos = funcstate->_unresolvedcontinues.back();
            funcstate->_unresolvedcontinues.pop_back();
            //set the jmp instruction
            funcstate->SetInstructionParams(pos, 0, targetpos - pos, 0);
            ntoresolve--;
        }
    }
private:
    SQInteger _token;               //!< 現在のトークン
    SQFuncState *_fs;               //!< 現在の関数状態へのポインタ
    SQObjectPtr _sourcename;        //!< ソース名
    SQLexer _lex;                   //!< 字句解析器インスタンス
    bool _lineinfo;                 //!< 行情報を生成するかどうかのフラグ
    bool _raiseerror;               //!< エラーハンドラを呼び出すかどうかのフラグ
    SQInteger _debugline;           //!< デバッグ用の行番号
    SQInteger _debugop;             //!< デバッグ用のオペコード
    SQExpState   _es;               //!< 現在の式の状態
    SQScope _scope;                 //!< 現在のスコープ情報
    SQChar _compilererror[MAX_COMPILER_ERROR_LEN]; //!< コンパイラエラーメッセージ用バッファ
    jmp_buf _errorjmp;              //!< エラー発生時のジャンプ先バッファ
    SQVM *_vm;                      //!< Squirrel VMへのポインタ
};

/**
 * @brief Squirrelスクリプトをコンパイルするグローバル関数。
 * @brief SQCompilerクラスのラッパーとして機能します。
 * @param vm 関連付けられるSquirrel VMへのポインタ。
 * @param rg 字句解析器がソースコードを読み込むための関数ポインタ。
 * @param up rg関数に渡されるユーザーポインタ。
 * @param sourcename コンパイル対象のソース名。
 * @param[out] out コンパイル結果として生成された関数プロトタイプの格納先。
 * @param raiseerror エラー発生時にエラーハンドラを呼び出すかどうか。
 * @param lineinfo バイトコードに行情報を含めるかどうか。
 * @return コンパイルが成功した場合はtrue、失敗した場合はfalse。
 */
bool Compile(SQVM *vm,SQLEXREADFUNC rg, SQUserPointer up, const SQChar *sourcename, SQObjectPtr &out, bool raiseerror, bool lineinfo)
{
    SQCompiler p(vm, rg, up, sourcename, raiseerror, lineinfo);
    return p.Compile(out);
}

#endif