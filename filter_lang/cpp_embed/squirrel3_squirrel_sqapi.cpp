//cpp

/*
    Written By OpenAI
*/
#include "sqpcheader.h"
#include "sqvm.h"
#include "sqstring.h"
#include "sqtable.h"
#include "sqarray.h"
#include "sqfuncproto.h"
#include "sqclosure.h"
#include "squserdata.h"
#include "sqcompiler.h"
#include "sqfuncstate.h"
#include "sqclass.h"

/**
 * @brief 引数の型をチェックし、指定の型と一致していれば対象オブジェクトを取得する。
 * 
 * @details
 * 仮想マシンスタックから指定インデックスのオブジェクトを取得し、与えられた型と一致するかを検証する。
 * 型が一致しない場合、エラーメッセージを生成してfalseを返す。
 * 
 * @param v スクリプト仮想マシン（VM）インスタンス。
 * @param idx スタック上の引数インデックス。
 * @param type 期待されるオブジェクト型。
 * @param[out] o 対応するオブジェクトが格納されるポインタへのポインタ。
 * 
 * @retval true 型が一致し、`*o` に対象が格納された場合。
 * @retval false 型が一致せず、エラーが発生した場合。
 */
static bool sq_aux_gettypedarg(HSQUIRRELVM v,SQInteger idx,SQObjectType type,SQObjectPtr **o)
{
    *o = &stack_get(v,idx);
    if(sq_type(**o) != type){
        SQObjectPtr oval = v->PrintObjVal(**o);
        v->Raise_Error(_SC("wrong argument type, expected '%s' got '%.50s'"),IdType2Name(type),_stringval(oval));
        return false;
    }
    return true;
}

/**
 * @brief 指定型のオブジェクトをスタックから安全に取得する。
 * 
 * @details
 * `sq_aux_gettypedarg()` を用いて、スタック上の値が指定された型であることを確認し、
 * 該当しない場合は `SQ_ERROR` を返すショートカットマクロ。
 * 
 * @param v スクリプト仮想マシン（VM）インスタンス。
 * @param idx スタックインデックス。
 * @param type 期待する `SQObjectType` 型。
 * @param o 結果として取得される `SQObjectPtr*` 変数名（変数そのものではない）。
 * 
 * @note `o` はアドレス渡しの変数名であり、呼び出し元で宣言されている必要があります。
 * @retval SQ_ERROR 型が一致しない場合。
 */
#define _GETSAFE_OBJ(v,idx,type,o) { if(!sq_aux_gettypedarg(v,idx,type,&o)) return SQ_ERROR; }

/**
 * @brief スタックに十分な数の引数があるかを検査する。
 * 
 * @details
 * 引数の数が `count` に満たない場合、エラーメッセージを設定して `SQ_ERROR` を返す。
 * マクロの展開先での早期リターンに使用される。
 * 
 * @param v スクリプト仮想マシン（VM）インスタンス。
 * @param count 最低限必要な引数の数。
 * 
 * @retval SQ_ERROR 引数が不足している場合。
 */
#define sq_aux_paramscheck(v,count) \
{ \
    if(sq_gettop(v) < count){ v->Raise_Error(_SC("not enough params in the stack")); return SQ_ERROR; }\
}

/**
 * @brief 想定外のオブジェクト型が指定された場合のエラーを返す。
 * 
 * @details
 * 指定された型情報を文字列に変換して、"unexpected type <型名>" というエラーメッセージを生成し、
 * `sq_throwerror()` を通じて例外として返す。
 * 
 * @param v スクリプト仮想マシン（VM）インスタンス。
 * @param type 想定外だった `SQObjectType`。
 * 
 * @return SQInteger エラーコード（常に `SQ_ERROR` を返す）。
 */
SQInteger sq_aux_invalidtype(HSQUIRRELVM v, SQObjectType type)
{
    SQUnsignedInteger buf_size = 100 * sizeof(SQChar);
    scsprintf(_ss(v)->GetScratchPad(buf_size), buf_size, _SC("unexpected type %s"), IdType2Name(type));
    return sq_throwerror(v, _ss(v)->GetScratchPad(-1));
}

/**
 * @brief 新しい Squirrel 仮想マシン（VM）インスタンスを作成し初期化する。
 * 
 * @details
 * 指定された初期スタックサイズで仮想マシンを生成する。
 * 内部的に `SQSharedState` を作成・初期化し、それに紐づいた `SQVM` を構築する。
 * 初期化が失敗した場合はメモリを解放して NULL を返す。
 * 
 * @param initialstacksize VMスタックの初期サイズ。
 * 
 * @return HSQUIRRELVM 成功時は初期化済みの仮想マシンインスタンス、失敗時は NULL。
 */
HSQUIRRELVM sq_open(SQInteger initialstacksize)
{
    SQSharedState *ss;
    SQVM *v;
    sq_new(ss, SQSharedState);
    ss->Init();
    v = (SQVM *)SQ_MALLOC(sizeof(SQVM));
    new (v) SQVM(ss);
    ss->_root_vm = v;
    if(v->Init(NULL, initialstacksize)) {
        return v;
    } else {
        sq_delete(v, SQVM);
        return NULL;
    }
    return v;
}

/**
 * @brief 新しいスレッドVM（子VM）を生成する。
 * 
 * @details
 * 指定された親VM (`friendvm`) をベースに共有状態を継承した新しい `SQVM` を生成する。
 * 成功すると生成されたスレッドVMを親VMのスタックにプッシュし、戻り値として返す。
 * 初期化に失敗した場合はメモリを解放して NULL を返す。
 * 
 * @param friendvm 親となる `HSQUIRRELVM` インスタンス。
 * @param initialstacksize 生成するスレッドVMのスタック初期サイズ。
 * 
 * @return HSQUIRRELVM 成功時は新しい子VM、失敗時は NULL。
 */
HSQUIRRELVM sq_newthread(HSQUIRRELVM friendvm, SQInteger initialstacksize)
{
    SQSharedState *ss;
    SQVM *v;
    ss=_ss(friendvm);

    v= (SQVM *)SQ_MALLOC(sizeof(SQVM));
    new (v) SQVM(ss);

    if(v->Init(friendvm, initialstacksize)) {
        friendvm->Push(v);
        return v;
    } else {
        sq_delete(v, SQVM);
        return NULL;
    }
}

/**
 * @brief 仮想マシン（VM）の現在の状態を取得する。
 * 
 * @details
 * VMが一時停止中（suspended）、実行中（running）、またはアイドル（idle）状態のいずれであるかを判定し、
 * 状態を表す定数を返す。
 * 
 * 状態は以下のいずれか：
 * - `SQ_VMSTATE_SUSPENDED`：実行が中断されている
 * - `SQ_VMSTATE_RUNNING`：現在関数呼び出し中
 * - `SQ_VMSTATE_IDLE`：何も実行されていない
 * 
 * @param v 対象となる仮想マシンインスタンス。
 * 
 * @return SQInteger VMの状態を表す定数。
 */
SQInteger sq_getvmstate(HSQUIRRELVM v)
{
    if(v->_suspended)
        return SQ_VMSTATE_SUSPENDED;
    else {
        if(v->_callsstacksize != 0) return SQ_VMSTATE_RUNNING;
        else return SQ_VMSTATE_IDLE;
    }
}

/**
 * @brief 仮想マシンにエラーハンドラを設定する。
 * 
 * @details
 * スタックのトップにあるオブジェクトをエラーハンドラとして設定する。
 * 対象はクロージャ（`closure`）、ネイティブクロージャ（`nativeclosure`）、または `null` のいずれかである必要がある。
 * 
 * 設定後はスタックのトップをポップする。
 * 
 * @param v 対象となる仮想マシンインスタンス。
 */
void sq_seterrorhandler(HSQUIRRELVM v)
{
    SQObject o = stack_get(v, -1);
    if(sq_isclosure(o) || sq_isnativeclosure(o) || sq_isnull(o)) {
        v->_errorhandler = o;
        v->Pop();
    }
}

/**
 * @brief ネイティブのデバッグフック関数を設定する。
 * 
 * @details
 * 指定された関数ポインタ `hook` を VM に設定し、ネイティブのデバッグ出力を有効にする。
 * 同時に、スクリプトレベルのデバッグクロージャ（`_debughook_closure`）はリセット（null）される。
 * 
 * @param v 対象となる仮想マシンインスタンス。
 * @param hook 設定するデバッグフック関数（関数ポインタ）。NULL の場合は無効化。
 */
void sq_setnativedebughook(HSQUIRRELVM v,SQDEBUGHOOK hook)
{
    v->_debughook_native = hook;
    v->_debughook_closure.Null();
    v->_debughook = hook?true:false;
}

/**
 * @brief スクリプトレベルのデバッグフッククロージャを設定する。
 * 
 * @details
 * スタックのトップにあるオブジェクトをデバッグフッククロージャとして設定する。
 * オブジェクトはクロージャ、ネイティブクロージャ、または `null` である必要がある。
 * 設定により、ネイティブのデバッグフックは無効化される（`_debughook_native = NULL`）。
 * 
 * 設定後はスタックのトップをポップする。
 * 
 * @param v 対象となる仮想マシンインスタンス。
 */
void sq_setdebughook(HSQUIRRELVM v)
{
    SQObject o = stack_get(v,-1);
    if(sq_isclosure(o) || sq_isnativeclosure(o) || sq_isnull(o)) {
        v->_debughook_closure = o;
        v->_debughook_native = NULL;
        v->_debughook = !sq_isnull(o);
        v->Pop();
    }
}

/**
 * @brief 仮想マシンを終了し、関連するリソースを解放する。
 * 
 * @details
 * ルートVMに対して `Finalize()` を呼び出し、共有状態オブジェクト（`SQSharedState`）を解放する。
 * この操作により、仮想マシンとそのリソースは完全に破棄される。
 * 
 * @param v 対象となる仮想マシンインスタンス。
 */
void sq_close(HSQUIRRELVM v)
{
    SQSharedState *ss = _ss(v);
    _thread(ss->_root_vm)->Finalize();
    sq_delete(ss, SQSharedState);
}

/**
 * @brief Squirrel 仮想マシンのバージョン番号を取得する。
 * 
 * @details
 * 定数 `SQUIRREL_VERSION_NUMBER` に定義されているバージョン番号を返す。
 * 通常、コンパイル時定義により決まる。
 * 
 * @return SQInteger Squirrel のバージョン番号。
 */
SQInteger sq_getversion()
{
    return SQUIRREL_VERSION_NUMBER;
}

/**
 * @brief スクリプトソースをコンパイルして、実行可能なクロージャをスタックにプッシュする。
 * 
 * @details
 * ソースコードを読み取るためのリーダー関数（`read`）を通じて入力を受け取り、
 * コンパイラを用いてバイトコードに変換し、関数クロージャとして `SQClosure` を生成・プッシュする。
 * 
 * `NO_COMPILER` が定義されている場合はビルド構成上コンパイル不可であり、エラーを返す。
 * 
 * @param v 対象となる仮想マシン。
 * @param read ソース入力用の関数ポインタ（1バイトずつ読み取る関数）。
 * @param p `read` 関数に渡されるユーザーデータ。
 * @param sourcename エラーメッセージなどに使用されるソース名。
 * @param raiseerror コンパイルエラー発生時に例外を投げるかどうかのフラグ。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR コンパイル失敗。
 */
SQRESULT sq_compile(HSQUIRRELVM v,SQLEXREADFUNC read,SQUserPointer p,const SQChar *sourcename,SQBool raiseerror)
{
    SQObjectPtr o;
#ifndef NO_COMPILER
    if(Compile(v, read, p, sourcename, o, raiseerror?true:false, _ss(v)->_debuginfo)) {
        v->Push(SQClosure::Create(_ss(v), _funcproto(o), _table(v->_roottable)->GetWeakRef(OT_TABLE)));
        return SQ_OK;
    }
    return SQ_ERROR;
#else
    return sq_throwerror(v,_SC("this is a no compiler build"));
#endif
}

/**
 * @brief コンパイル時にデバッグ情報を埋め込むかどうかを設定する。
 * 
 * @details
 * デバッグ情報には、関数名・ファイル名・行番号などの情報が含まれ、
 * スタックトレースの出力やデバッガによる解析に必要となる。
 * この設定は仮想マシンの共有状態に対して適用される。
 * 
 * @param v 対象となる仮想マシン。
 * @param enable 有効にする場合は true、無効にする場合は false。
 */
void sq_enabledebuginfo(HSQUIRRELVM v, SQBool enable)
{
    _ss(v)->_debuginfo = enable?true:false;
}

/**
 * @brief すべての例外に対して通知を行うかどうかを設定する。
 * 
 * @details
 * 通常、通知は未捕捉の例外に対してのみ行われるが、このフラグを有効にすることで
 * 捕捉された例外も含めて通知対象にできる。
 * 
 * この設定は共有状態 `_notifyallexceptions` に保存され、VMの例外処理挙動に影響を与える。
 * 
 * @param v 対象となる仮想マシン。
 * @param enable 有効にする場合は true、無効にする場合は false。
 */
void sq_notifyallexceptions(HSQUIRRELVM v, SQBool enable)
{
    _ss(v)->_notifyallexceptions = enable?true:false;
}

/**
 * @brief 指定されたオブジェクトの参照カウントを1つ増加させる。
 * 
 * @details
 * 参照カウントを持つオブジェクト（文字列、配列、テーブルなど）に対して、
 * 明示的にリファレンスを保持したい場合に使用する。
 * 
 * `NO_GARBAGE_COLLECTOR` が定義されている場合、ビルドに応じて異なる処理が行われる。
 * 
 * @param v 対象となる仮想マシン。
 * @param po 対象の `HSQOBJECT`。型が参照カウント対象でない場合は無視される。
 */
void sq_addref(HSQUIRRELVM v,HSQOBJECT *po)
{
    if(!ISREFCOUNTED(sq_type(*po))) return;
#ifdef NO_GARBAGE_COLLECTOR
    __AddRef(po->_type,po->_unVal);
#else
    _ss(v)->_refs_table.AddRef(*po);
#endif
}

/**
 * @brief 指定されたオブジェクトの参照カウントを取得する。
 * 
 * @details
 * 参照カウントを持つオブジェクト（例：配列、テーブル、クロージャなど）に対して、
 * 現在の参照数（retain count）を返す。対象が参照カウント対象でない場合は 0 を返す。
 * 
 * `NO_GARBAGE_COLLECTOR` が定義されている場合とそうでない場合で取得方法が異なる。
 * 
 * @param v 対象の仮想マシン。
 * @param po 対象の `HSQOBJECT` ポインタ。
 * 
 * @return SQUnsignedInteger 現在の参照カウント。対象外であれば 0。
 */
SQUnsignedInteger sq_getrefcount(HSQUIRRELVM v,HSQOBJECT *po)
{
    if(!ISREFCOUNTED(sq_type(*po))) return 0;
#ifdef NO_GARBAGE_COLLECTOR
   return po->_unVal.pRefCounted->_uiRef;
#else
   return _ss(v)->_refs_table.GetRefCount(*po);
#endif
}

/**
 * @brief 指定されたオブジェクトの参照カウントを減少させ、必要に応じて解放する。
 * 
 * @details
 * ガーベジコレクションが有効でない構成では、参照カウントが1以下の場合に解放される。
 * 
 * ガーベジコレクションが有効な場合は、`_refs_table` に管理を委ねて `Release()` を呼び出す。
 * 
 * @param v 対象の仮想マシン。
 * @param po 対象の `HSQOBJECT` ポインタ。
 * 
 * @retval SQTrue 解放されたか、そもそも参照カウントを持たないオブジェクトだった場合。
 * @retval SQFalse ガーベジコレクタなし構成で解放されなかった場合（※副作用なし）。
 */
SQBool sq_release(HSQUIRRELVM v,HSQOBJECT *po)
{
    if(!ISREFCOUNTED(sq_type(*po))) return SQTrue;
#ifdef NO_GARBAGE_COLLECTOR
    bool ret = (po->_unVal.pRefCounted->_uiRef <= 1) ? SQTrue : SQFalse;
    __Release(po->_type,po->_unVal);
    return ret; //the ret val doesn't work(and cannot be fixed)
#else
    return _ss(v)->_refs_table.Release(*po);
#endif
}

/**
 * @brief 指定されたオブジェクトの参照カウント値を直接取得する（VM外の補助関数）。
 * 
 * @details
 * この関数は、ガーベジコレクションに関係なくオブジェクトの内部 `RefCount` を直接参照して返す。
 * 通常の参照管理と無関係に、デバッグや監視の目的で使用される。
 * 
 * @param v 仮想マシン（未使用だが引数として必要）。
 * @param po 対象の `HSQOBJECT` ポインタ。
 * 
 * @return SQUnsignedInteger 参照カウント。参照管理対象でない場合は 0。
 */
SQUnsignedInteger sq_getvmrefcount(HSQUIRRELVM SQ_UNUSED_ARG(v), const HSQOBJECT *po)
{
    if (!ISREFCOUNTED(sq_type(*po))) return 0;
    return po->_unVal.pRefCounted->_uiRef;
}

/**
 * @brief オブジェクトを文字列型として取得する。
 * 
 * @details
 * 指定された `HSQOBJECT` が文字列（`OT_STRING`）型であれば、
 * その文字列へのポインタを返す。それ以外の型であれば NULL を返す。
 * 
 * @param o 対象の `HSQOBJECT` ポインタ。
 * 
 * @return const SQChar* オブジェクトが文字列型であればその文字列、そうでなければ NULL。
 */
const SQChar *sq_objtostring(const HSQOBJECT *o)
{
    if(sq_type(*o) == OT_STRING) {
        return _stringval(*o);
    }
    return NULL;
}

/**
 * @brief オブジェクトを整数（`SQInteger`）として取得する。
 * 
 * @details
 * 指定された `HSQOBJECT` が数値型（整数または浮動小数）であれば、
 * 整数に変換して返す。それ以外の型の場合は 0 を返す。
 * 
 * @param o 対象の `HSQOBJECT` ポインタ。
 * 
 * @return SQInteger 数値型であればその整数値、そうでなければ 0。
 */
SQInteger sq_objtointeger(const HSQOBJECT *o)
{
    if(sq_isnumeric(*o)) {
        return tointeger(*o);
    }
    return 0;
}

/**
 * @brief オブジェクトを浮動小数点数（`SQFloat`）として取得する。
 * 
 * @details
 * 指定されたオブジェクトが数値型（整数または浮動小数点）であれば、
 * 浮動小数に変換して返す。非数値型の場合は 0.0 を返す。
 * 
 * @param o 対象の `HSQOBJECT` ポインタ。
 * 
 * @return SQFloat 浮動小数型での値。非数値型の場合は 0.0。
 */
SQFloat sq_objtofloat(const HSQOBJECT *o)
{
    if(sq_isnumeric(*o)) {
        return tofloat(*o);
    }
    return 0;
}

/**
 * @brief オブジェクトをブール値（`SQBool`）として取得する。
 * 
 * @details
 * オブジェクトがブール型である場合、その値を返す。
 * ブール型でない場合は `SQFalse` を返す。
 * 
 * @param o 対象の `HSQOBJECT` ポインタ。
 * 
 * @return SQBool ブール値（`SQTrue` または `SQFalse`）。
 */
SQBool sq_objtobool(const HSQOBJECT *o)
{
    if(sq_isbool(*o)) {
        return _integer(*o);
    }
    return SQFalse;
}

/**
 * @brief オブジェクトをユーザーポインタとして取得する。
 * 
 * @details
 * 指定されたオブジェクトがユーザーポインタ型（`OT_USERPOINTER`）である場合、
 * ポインタ値を返す。それ以外の型であれば 0 を返す。
 * 
 * @param o 対象の `HSQOBJECT` ポインタ。
 * 
 * @return SQUserPointer オブジェクトがユーザーポインタ型であればその値、そうでなければ 0。
 */
SQUserPointer sq_objtouserpointer(const HSQOBJECT *o)
{
    if(sq_isuserpointer(*o)) {
        return _userpointer(*o);
    }
    return 0;
}

/**
 * @brief スタックに `null` 値をプッシュする。
 * 
 * @details
 * 対象の仮想マシンのスタックトップに `null` を追加する。
 * 
 * @param v 対象の仮想マシン。
 */
void sq_pushnull(HSQUIRRELVM v)
{
    v->PushNull();
}

/**
 * @brief スタックに文字列をプッシュする。
 * 
 * @details
 * 指定された文字列（`s`）を仮想マシンのスタックにプッシュする。
 * 文字列が NULL の場合は `null` をプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param s プッシュする文字列へのポインタ。NULLの場合は `null` をプッシュ。
 * @param len 文字列の長さ（バイト数）。
 */
void sq_pushstring(HSQUIRRELVM v,const SQChar *s,SQInteger len)
{
    if(s)
        v->Push(SQObjectPtr(SQString::Create(_ss(v), s, len)));
    else v->PushNull();
}

/**
 * @brief スタックに整数値をプッシュする。
 * 
 * @details
 * 指定された整数値を仮想マシンのスタック上にプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param n プッシュする整数値。
 */
void sq_pushinteger(HSQUIRRELVM v,SQInteger n)
{
    v->Push(n);
}

/**
 * @brief スタックにブール値をプッシュする。
 * 
 * @details
 * 指定されたブール値（true/false）を、仮想マシンのスタックにプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param b プッシュするブール値（`SQTrue` または `SQFalse`）。
 */
void sq_pushbool(HSQUIRRELVM v,SQBool b)
{
    v->Push(b?true:false);
}

/**
 * @brief スタックに浮動小数点数をプッシュする。
 * 
 * @details
 * 指定された `SQFloat` 型の値を仮想マシンのスタックにプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param n プッシュする浮動小数点値。
 */
void sq_pushfloat(HSQUIRRELVM v,SQFloat n)
{
    v->Push(n);
}

/**
 * @brief スタックにユーザーポインタをプッシュする。
 * 
 * @details
 * 任意のポインタ値を `userpointer` 型として仮想マシンのスタックにプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param p プッシュするユーザーポインタ。
 */
void sq_pushuserpointer(HSQUIRRELVM v,SQUserPointer p)
{
    v->Push(p);
}

/**
 * @brief スレッド（VMインスタンス）をスタックにプッシュする。
 * 
 * @details
 * 指定されたスレッド型の仮想マシンインスタンスを、`thread` 型として
 * 対象 VM のスタックにプッシュする。
 * 
 * @param v スレッドをプッシュする側の仮想マシン。
 * @param thread プッシュされるスレッド型仮想マシン。
 */
void sq_pushthread(HSQUIRRELVM v, HSQUIRRELVM thread)
{
    v->Push(thread);
}

/**
 * @brief 新しいユーザーデータ領域を確保してスタックにプッシュする。
 * 
 * @details
 * 指定サイズのユーザーデータ（`SQUserData`）を確保し、アライメントを考慮して
 * 有効なデータ領域の先頭ポインタを返す。データはスタックにもプッシュされる。
 * 
 * @param v 対象の仮想マシン。
 * @param size 確保するユーザーデータ領域のサイズ（バイト単位）。
 * 
 * @return SQUserPointer 有効なユーザーデータへのポインタ。
 */
SQUserPointer sq_newuserdata(HSQUIRRELVM v,SQUnsignedInteger size)
{
    SQUserData *ud = SQUserData::Create(_ss(v), size + SQ_ALIGNMENT);
    v->Push(ud);
    return (SQUserPointer)sq_aligning(ud + 1);
}

/**
 * @brief 新しい空のテーブルを生成してスタックにプッシュする。
 * 
 * @details
 * 初期容量0のハッシュテーブルを作成し、対象仮想マシンのスタックにプッシュする。
 * 
 * @param v 対象の仮想マシン。
 */
void sq_newtable(HSQUIRRELVM v)
{
    v->Push(SQTable::Create(_ss(v), 0));
}

/**
 * @brief 指定した初期容量で新しいテーブルを生成し、スタックにプッシュする。
 * 
 * @details
 * パフォーマンス最適化のため、事前に想定される要素数を指定して
 * ハッシュテーブルの初期サイズを設定する。
 * 
 * @param v 対象の仮想マシン。
 * @param initialcapacity テーブルの初期要素数（ハッシュサイズの目安）。
 */
void sq_newtableex(HSQUIRRELVM v,SQInteger initialcapacity)
{
    v->Push(SQTable::Create(_ss(v), initialcapacity));
}

/**
 * @brief 指定されたサイズの配列を生成し、スタックにプッシュする。
 * 
 * @details
 * 空の要素で初期化された配列を生成し、対象の仮想マシンスタックに追加する。
 * 
 * @param v 対象の仮想マシン。
 * @param size 初期化される配列のサイズ（要素数）。
 */
void sq_newarray(HSQUIRRELVM v,SQInteger size)
{
    v->Push(SQArray::Create(_ss(v), size));
}

/**
 * @brief 新しいクラスを生成し、スタックにプッシュする。
 * 
 * @details
 * 指定があれば、既存のクラスを継承した新しいクラスを生成する。
 * 
 * - `hasbase` が true の場合、スタック上のトップにあるクラスを継承元として使用する。
 * - `hasbase` が false の場合は継承なしの空クラスが作成される。
 * 
 * クラス生成後、スタックにプッシュされる。
 * 
 * @param v 対象の仮想マシン。
 * @param hasbase ベースクラスを使用するかどうかのフラグ。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR ベース型が無効だった場合。
 */
SQRESULT sq_newclass(HSQUIRRELVM v,SQBool hasbase)
{
    SQClass *baseclass = NULL;
    if(hasbase) {
        SQObjectPtr &base = stack_get(v,-1);
        if(sq_type(base) != OT_CLASS)
            return sq_throwerror(v,_SC("invalid base type"));
        baseclass = _class(base);
    }
    SQClass *newclass = SQClass::Create(_ss(v), baseclass);
    if(baseclass) v->Pop();
    v->Push(newclass);
    return SQ_OK;
}

/**
 * @brief オブジェクトが指定されたクラスのインスタンスかどうかを判定する。
 * 
 * @details
 * スタック上の -1（トップ）がインスタンス、-2 がクラスである必要がある。
 * インスタンスがそのクラス、またはその派生クラスであるかを検査する。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @retval SQTrue インスタンスがクラス、またはその派生クラスのものである場合。
 * @retval SQ_ERROR 引数の型が不正な場合。
 */
SQBool sq_instanceof(HSQUIRRELVM v)
{
    SQObjectPtr &inst = stack_get(v,-1);
    SQObjectPtr &cl = stack_get(v,-2);
    if(sq_type(inst) != OT_INSTANCE || sq_type(cl) != OT_CLASS)
        return sq_throwerror(v,_SC("invalid param type"));
    return _instance(inst)->InstanceOf(_class(cl))?SQTrue:SQFalse;
}

/**
 * @brief 指定した配列の末尾に要素を追加する。
 * 
 * @details
 * スタックのトップにある要素を、`idx` で指定された配列の末尾に追加する。
 * 追加後、スタックトップから要素は削除される。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 配列が存在するスタックインデックス。
 * 
 * @retval SQ_OK 正常に追加された場合。
 * @retval SQ_ERROR 対象が配列でない場合、またはパラメータ数が不足している場合。
 */
SQRESULT sq_arrayappend(HSQUIRRELVM v,SQInteger idx)
{
    sq_aux_paramscheck(v,2);
    SQObjectPtr *arr;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,arr);
    _array(*arr)->Append(v->GetUp(-1));
    v->Pop();
    return SQ_OK;
}

/**
 * @brief 指定した配列の末尾の要素を削除する。
 * 
 * @details
 * 指定された配列の末尾要素を削除する。`pushval` が true の場合、
 * 削除する前の要素をスタックにプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象の配列のスタックインデックス。
 * @param pushval 削除前の要素をスタックにプッシュするかどうか。
 * 
 * @retval SQ_OK 正常に削除された場合。
 * @retval SQ_ERROR 配列が空の場合、または引数が不正な場合。
 */
SQRESULT sq_arraypop(HSQUIRRELVM v,SQInteger idx,SQBool pushval)
{
    sq_aux_paramscheck(v, 1);
    SQObjectPtr *arr;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,arr);
    if(_array(*arr)->Size() > 0) {
        if(pushval != 0){ v->Push(_array(*arr)->Top()); }
        _array(*arr)->Pop();
        return SQ_OK;
    }
    return sq_throwerror(v, _SC("empty array"));
}

/**
 * @brief 指定した配列のサイズを変更する。
 * 
 * @details
 * 配列のサイズを `newsize` に変更する。新しいサイズが現在より大きい場合は
 * `null` で埋められ、小さい場合は末尾から要素が削除される。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象の配列のスタックインデックス。
 * @param newsize 新しい配列のサイズ（0以上）。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR newsize が負の値だった場合、または対象が配列でない場合。
 */

SQRESULT sq_arrayresize(HSQUIRRELVM v,SQInteger idx,SQInteger newsize)
{
    sq_aux_paramscheck(v,1);
    SQObjectPtr *arr;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,arr);
    if(newsize >= 0) {
        _array(*arr)->Resize(newsize);
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("negative size"));
}

/**
 * @brief 配列内の要素の順序を反転する。
 * 
 * @details
 * 配列の要素をインプレースで反転する（先頭と末尾を交換、次にその内側...）。
 * 配列が空または1要素のみの場合は何も行わない。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象の配列のスタックインデックス。
 * 
 * @retval SQ_OK 成功（反転したか、何もしなかったか）。
 * @retval SQ_ERROR 対象が配列でない、または引数が不正な場合。
 */
SQRESULT sq_arrayreverse(HSQUIRRELVM v,SQInteger idx)
{
    sq_aux_paramscheck(v, 1);
    SQObjectPtr *o;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,o);
    SQArray *arr = _array(*o);
    if(arr->Size() > 0) {
        SQObjectPtr t;
        SQInteger size = arr->Size();
        SQInteger n = size >> 1; size -= 1;
        for(SQInteger i = 0; i < n; i++) {
            t = arr->_values[i];
            arr->_values[i] = arr->_values[size-i];
            arr->_values[size-i] = t;
        }
        return SQ_OK;
    }
    return SQ_OK;
}

/**
 * @brief 配列から指定されたインデックスの要素を削除する。
 * 
 * @details
 * 配列の `itemidx` で指定された位置の要素を削除し、後続の要素が前に詰められる。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象の配列のスタックインデックス。
 * @param itemidx 削除する要素のインデックス。
 * 
 * @retval SQ_OK 成功（削除された場合）。
 * @retval SQ_ERROR 配列以外の型、またはインデックス範囲外の場合。
 */
SQRESULT sq_arrayremove(HSQUIRRELVM v,SQInteger idx,SQInteger itemidx)
{
    sq_aux_paramscheck(v, 1);
    SQObjectPtr *arr;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,arr);
    return _array(*arr)->Remove(itemidx) ? SQ_OK : sq_throwerror(v,_SC("index out of range"));
}

/**
 * @brief 配列の指定位置に要素を挿入する。
 * 
 * @details
 * スタックのトップにある値を、`destpos` で指定された配列のインデックス位置に挿入する。
 * 挿入後、スタックトップは自動的にポップされる。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象の配列のスタックインデックス。
 * @param destpos 挿入する位置（インデックス）。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 配列以外が指定された場合や、`destpos` が範囲外の場合。
 */
SQRESULT sq_arrayinsert(HSQUIRRELVM v,SQInteger idx,SQInteger destpos)
{
    sq_aux_paramscheck(v, 1);
    SQObjectPtr *arr;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,arr);
    SQRESULT ret = _array(*arr)->Insert(destpos, v->GetUp(-1)) ? SQ_OK : sq_throwerror(v,_SC("index out of range"));
    v->Pop();
    return ret;
}

/**
 * @brief 新しいネイティブクロージャを作成してスタックにプッシュする。
 * 
 * @details
 * 指定された C 関数ポインタ `func` と、自由変数の数 `nfreevars` を元に、
 * ネイティブクロージャ（`SQNativeClosure`）を生成し、スタックにプッシュする。
 * 
 * スタックのトップから `nfreevars` 個の値が自由変数として取り出され、クロージャにバインドされる。
 * 
 * @param v 対象の仮想マシン。
 * @param func ネイティブ関数のポインタ。
 * @param nfreevars クロージャにバインドする自由変数の個数。
 */
void sq_newclosure(HSQUIRRELVM v,SQFUNCTION func,SQUnsignedInteger nfreevars)
{
    SQNativeClosure *nc = SQNativeClosure::Create(_ss(v), func,nfreevars);
    nc->_nparamscheck = 0;
    for(SQUnsignedInteger i = 0; i < nfreevars; i++) {
        nc->_outervalues[i] = v->Top();
        v->Pop();
    }
    v->Push(SQObjectPtr(nc));
}

/**
 * @brief クロージャの引数数および自由変数数を取得する。
 * 
 * @details
 * 指定されたスタックインデックスのオブジェクトがクロージャ型の場合に、
 * 引数数と自由変数数を取得して出力引数に格納する。
 * 
 * - 通常クロージャ（`OT_CLOSURE`）とネイティブクロージャ（`OT_NATIVECLOSURE`）に対応。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象クロージャのスタックインデックス。
 * @param[out] nparams クロージャの引数数が格納される。
 * @param[out] nfreevars 自由変数の数が格納される。
 * 
 * @retval SQ_OK 情報の取得に成功。
 * @retval SQ_ERROR 対象がクロージャ型でない場合。
 */
SQRESULT sq_getclosureinfo(HSQUIRRELVM v,SQInteger idx,SQInteger *nparams,SQInteger *nfreevars)
{
    SQObject o = stack_get(v, idx);
    if(sq_type(o) == OT_CLOSURE) {
        SQClosure *c = _closure(o);
        SQFunctionProto *proto = c->_function;
        *nparams = proto->_nparameters;
        *nfreevars = proto->_noutervalues;
        return SQ_OK;
    }
    else if(sq_type(o) == OT_NATIVECLOSURE)
    {
        SQNativeClosure *c = _nativeclosure(o);
        *nparams = c->_nparamscheck;
        *nfreevars = (SQInteger)c->_noutervalues;
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("the object is not a closure"));
}

/**
 * @brief ネイティブクロージャに名前を設定する。
 * 
 * @details
 * スタックの指定インデックスにあるネイティブクロージャ（`OT_NATIVECLOSURE`）に
 * 与えられた名前文字列を設定する。
 * 
 * この名前はデバッグ時の表示や識別に使用される。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象クロージャのスタックインデックス。
 * @param name 設定する名前文字列。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がネイティブクロージャでない場合。
 */
SQRESULT sq_setnativeclosurename(HSQUIRRELVM v,SQInteger idx,const SQChar *name)
{
    SQObject o = stack_get(v, idx);
    if(sq_isnativeclosure(o)) {
        SQNativeClosure *nc = _nativeclosure(o);
        nc->_name = SQString::Create(_ss(v),name);
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("the object is not a nativeclosure"));
}

/**
 * @brief ネイティブクロージャの引数チェックルールを設定する。
 * 
 * @details
 * スタックのトップにあるネイティブクロージャに対して、期待される引数の個数（`nparamscheck`）と、
 * 型の制限を示すマスク文字列（`typemask`）を設定する。
 * 
 * typemask 例：
 * - `"iis"` → int, int, string
 * 
 * `SQ_MATCHTYPEMASKSTRING` を引数数に設定すると、マスクの長さから引数数が自動的に決定される。
 * 
 * @param v 対象の仮想マシン。
 * @param nparamscheck 引数の期待個数。`SQ_MATCHTYPEMASKSTRING` を指定することでマスクと一致させる。
 * @param typemask 型制約を示すマスク文字列（省略可能）。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クロージャでない、または typemask が無効な場合。
 */
SQRESULT sq_setparamscheck(HSQUIRRELVM v,SQInteger nparamscheck,const SQChar *typemask)
{
    SQObject o = stack_get(v, -1);
    if(!sq_isnativeclosure(o))
        return sq_throwerror(v, _SC("native closure expected"));
    SQNativeClosure *nc = _nativeclosure(o);
    nc->_nparamscheck = nparamscheck;
    if(typemask) {
        SQIntVec res;
        if(!CompileTypemask(res, typemask))
            return sq_throwerror(v, _SC("invalid typemask"));
        nc->_typecheck.copy(res);
    }
    else {
        nc->_typecheck.resize(0);
    }
    if(nparamscheck == SQ_MATCHTYPEMASKSTRING) {
        nc->_nparamscheck = nc->_typecheck.size();
    }
    return SQ_OK;
}

/**
 * @brief クロージャに環境オブジェクト（env）をバインドする。
 * 
 * @details
 * スタックの `idx` にあるクロージャに対して、トップにある環境オブジェクト（テーブル、配列、クラス、インスタンス）をバインドする。
 * 
 * 環境は弱参照として内部に保持される。対象がネイティブクロージャまたは通常クロージャでない場合はエラー。
 * 
 * @param v 対象の仮想マシン。
 * @param idx クロージャのスタックインデックス。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がクロージャでない、または無効な環境が指定された場合。
 */
SQRESULT sq_bindenv(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &o = stack_get(v,idx);
    if(!sq_isnativeclosure(o) &&
        !sq_isclosure(o))
        return sq_throwerror(v,_SC("the target is not a closure"));
    SQObjectPtr &env = stack_get(v,-1);
    if(!sq_istable(env) &&
        !sq_isarray(env) &&
        !sq_isclass(env) &&
        !sq_isinstance(env))
        return sq_throwerror(v,_SC("invalid environment"));
    SQWeakRef *w = _refcounted(env)->GetWeakRef(sq_type(env));
    SQObjectPtr ret;
    if(sq_isclosure(o)) {
        SQClosure *c = _closure(o)->Clone();
        __ObjRelease(c->_env);
        c->_env = w;
        __ObjAddRef(c->_env);
        if(_closure(o)->_base) {
            c->_base = _closure(o)->_base;
            __ObjAddRef(c->_base);
        }
        ret = c;
    }
    else { //then must be a native closure
        SQNativeClosure *c = _nativeclosure(o)->Clone();
        __ObjRelease(c->_env);
        c->_env = w;
        __ObjAddRef(c->_env);
        ret = c;
    }
    v->Pop();
    v->Push(ret);
    return SQ_OK;
}

/**
 * @brief クロージャに設定されている名前を取得する。
 * 
 * @details
 * 指定インデックスのオブジェクトがクロージャ（ネイティブまたは通常）である場合に、
 * その名前をスタックにプッシュする。名前が設定されていない場合は `null` が返される可能性もある。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象クロージャのスタックインデックス。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がクロージャ型でない場合。
 */
SQRESULT sq_getclosurename(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &o = stack_get(v,idx);
    if(!sq_isnativeclosure(o) &&
        !sq_isclosure(o))
        return sq_throwerror(v,_SC("the target is not a closure"));
    if(sq_isnativeclosure(o))
    {
        v->Push(_nativeclosure(o)->_name);
    }
    else { //closure
        v->Push(_closure(o)->_function->_name);
    }
    return SQ_OK;
}

/**
 * @brief クロージャにルートテーブルを設定する。
 * 
 * @details
 * 指定されたクロージャ（`OT_CLOSURE`）に対して、新しいルートテーブルを設定する。
 * 
 * スタックの `-1` にあるオブジェクトが `table` 型である必要がある。
 * 設定後はそのオブジェクトはポップされる。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象のクロージャのスタックインデックス。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がクロージャでない、または与えられたルートがテーブルでない場合。
 */
SQRESULT sq_setclosureroot(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &c = stack_get(v,idx);
    SQObject o = stack_get(v, -1);
    if(!sq_isclosure(c)) return sq_throwerror(v, _SC("closure expected"));
    if(sq_istable(o)) {
        _closure(c)->SetRoot(_table(o)->GetWeakRef(OT_TABLE));
        v->Pop();
        return SQ_OK;
    }
    return sq_throwerror(v, _SC("invalid type"));
}

/**
 * @brief クロージャに設定されているルートテーブルを取得する。
 * 
 * @details
 * 指定されたクロージャ（`OT_CLOSURE`）のルートオブジェクトを取得し、
 * スタックにプッシュする。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 対象クロージャのスタックインデックス。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がクロージャでない場合。
 */
SQRESULT sq_getclosureroot(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &c = stack_get(v,idx);
    if(!sq_isclosure(c)) return sq_throwerror(v, _SC("closure expected"));
    v->Push(_closure(c)->_root->_obj);
    return SQ_OK;
}

/**
 * @brief テーブルまたは配列の内容をすべて削除する。
 * 
 * @details
 * 指定インデックスのオブジェクトがテーブルなら `Clear()` を、
 * 配列なら `Resize(0)` によって要素を全消去する。
 * 対象がどちらでもない場合はエラーを返す。
 * 
 * @param v 対象の仮想マシン。
 * @param idx テーブルまたは配列が存在するスタックインデックス。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がテーブルでも配列でもない場合。
 */
SQRESULT sq_clear(HSQUIRRELVM v,SQInteger idx)
{
    SQObject &o=stack_get(v,idx);
    switch(sq_type(o)) {
        case OT_TABLE: _table(o)->Clear();  break;
        case OT_ARRAY: _array(o)->Resize(0); break;
        default:
            return sq_throwerror(v, _SC("clear only works on table and array"));
        break;

    }
    return SQ_OK;
}

/**
 * @brief 現在の仮想マシンに設定されているルートテーブルをスタックにプッシュする。
 * 
 * @details
 * ルートテーブルは、グローバルスコープに相当するテーブルであり、
 * スクリプトの変数や関数の格納先となる。
 * 
 * @param v 対象の仮想マシン。
 */
void sq_pushroottable(HSQUIRRELVM v)
{
    v->Push(v->_roottable);
}

/**
 * @brief レジストリテーブルをスタックにプッシュする。
 * 
 * @details
 * レジストリテーブルは VM の内部管理用途に使用される特殊なテーブルであり、
 * 外部からのデータ保存・共有などに使用できる。
 * 
 * @param v 対象の仮想マシン。
 */
void sq_pushregistrytable(HSQUIRRELVM v)
{
    v->Push(_ss(v)->_registry);
}

/**
 * @brief 定数（const）テーブルをスタックにプッシュする。
 * 
 * @details
 * 定数テーブルには予約定数やシステム定義の値などが格納されている。
 * これは読み取り専用であり、ユーザーによる直接編集は推奨されない。
 * 
 * @param v 対象の仮想マシン。
 */
void sq_pushconsttable(HSQUIRRELVM v)
{
    v->Push(_ss(v)->_consts);
}

/**
 * @brief 仮想マシンに新しいルートテーブルを設定する。
 * 
 * @details
 * スタックのトップにあるテーブルまたは null を、
 * 仮想マシンのグローバルルートテーブルとして設定する。
 * 
 * 設定後、スタックのトップ要素はポップされる。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がテーブルでも null でもない場合。
 */
SQRESULT sq_setroottable(HSQUIRRELVM v)
{
    SQObject o = stack_get(v, -1);
    if(sq_istable(o) || sq_isnull(o)) {
        v->_roottable = o;
        v->Pop();
        return SQ_OK;
    }
    return sq_throwerror(v, _SC("invalid type"));
}

/**
 * @brief 仮想マシンに新しい定数テーブルを設定する。
 * 
 * @details
 * スタックのトップにあるテーブルを、共有状態 `_consts` に設定する。
 * 定数テーブルには予約語や定義済みの値などが格納される。
 * 
 * 設定後、スタックトップはポップされる。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR スタックトップがテーブル型でない場合。
 */
SQRESULT sq_setconsttable(HSQUIRRELVM v)
{
    SQObject o = stack_get(v, -1);
    if(sq_istable(o)) {
        _ss(v)->_consts = o;
        v->Pop();
        return SQ_OK;
    }
    return sq_throwerror(v, _SC("invalid type, expected table"));
}

/**
 * @brief 仮想マシンに外部ポインタを関連付ける。
 * 
 * @details
 * 指定された任意のユーザーポインタを仮想マシンに設定する。
 * このポインタは、C/C++ 側で VM に関連する追加情報を保持したい場合などに使用される。
 * 
 * @param v 対象の仮想マシン。
 * @param p 任意のユーザーポインタ。
 */
void sq_setforeignptr(HSQUIRRELVM v,SQUserPointer p)
{
    v->_foreignptr = p;
}

/**
 * @brief 仮想マシンに関連付けられている外部ポインタを取得する。
 * 
 * @details
 * `sq_setforeignptr()` で設定された外部ポインタの値を返す。
 * 外部アプリケーションが VM に任意のコンテキスト情報を紐づけたい場合に使用される。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @return SQUserPointer 設定された外部ポインタ。未設定の場合は NULL。
 */
SQUserPointer sq_getforeignptr(HSQUIRRELVM v)
{
    return v->_foreignptr;
}

/**
 * @brief 共有状態に外部ポインタを設定する。
 * 
 * @details
 * このポインタはすべての VM インスタンスに共有される。
 * VM 単体の `sq_setforeignptr()` とは異なり、グローバルに有効な外部情報を保持したい場合に使用する。
 * 
 * @param v 対象の仮想マシン。
 * @param p 任意のユーザーポインタ。
 */
void sq_setsharedforeignptr(HSQUIRRELVM v,SQUserPointer p)
{
    _ss(v)->_foreignptr = p;
}

/**
 * @brief 共有状態に設定された外部ポインタを取得する。
 * 
 * @details
 * `sq_setsharedforeignptr()` で設定されたポインタを取得する。
 * これは全仮想マシンに共通する外部情報を格納する用途に使用される。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @return SQUserPointer 共有外部ポインタ。未設定の場合は NULL。
 */
SQUserPointer sq_getsharedforeignptr(HSQUIRRELVM v)
{
    return _ss(v)->_foreignptr;
}

/**
 * @brief 仮想マシンの解放時に実行されるリリースフックを設定する。
 * 
 * @details
 * VM が破棄される際にコールされるユーザー定義の解放処理（フック）関数を登録する。
 * VMのスコープ内で外部リソース管理を行う用途などに便利。
 * 
 * @param v 対象の仮想マシン。
 * @param hook 解放時に呼び出す関数ポインタ。
 */
void sq_setvmreleasehook(HSQUIRRELVM v,SQRELEASEHOOK hook)
{
    v->_releasehook = hook;
}

/**
 * @brief 設定されている仮想マシンのリリースフック関数を取得する。
 * 
 * @details
 * `sq_setvmreleasehook()` によって設定された VM の終了時フック関数を返す。
 * フックが未設定の場合は NULL を返す。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @return SQRELEASEHOOK 現在設定されているリリースフック関数。
 */
SQRELEASEHOOK sq_getvmreleasehook(HSQUIRRELVM v)
{
    return v->_releasehook;
}

/**
 * @brief 共有状態のリリースフック関数を設定する。
 * 
 * @details
 * VM の共有領域（`SharedState`）が解放される際に実行されるフック関数を指定する。
 * これは複数の VM 間で共有されるリソースの管理や後処理に使用される。
 * 
 * @param v 対象の仮想マシン。
 * @param hook 共有状態が破棄されるときに呼ばれる関数ポインタ。
 */
void sq_setsharedreleasehook(HSQUIRRELVM v,SQRELEASEHOOK hook)
{
    _ss(v)->_releasehook = hook;
}

/**
 * @brief 共有状態に設定されているリリースフック関数を取得する。
 * 
 * @details
 * `sq_setsharedreleasehook()` で設定された共有領域の解放フック関数を取得する。
 * フック関数が設定されていない場合は NULL を返す。
 * 
 * @param v 対象の仮想マシン。
 * 
 * @return SQRELEASEHOOK 現在設定されている共有リリースフック関数。
 */
SQRELEASEHOOK sq_getsharedreleasehook(HSQUIRRELVM v)
{
    return _ss(v)->_releasehook;
}

/**
 * @brief スタックに値をプッシュする。
 * 
 * @details
 * 指定インデックス `idx` にある値をスタックトップに複製してプッシュする。
 * インデックスは負値でスタック末尾からの相対指定も可能。
 * 
 * @param v 対象の仮想マシン。
 * @param idx スタックから複製する値のインデックス。
 */
void sq_push(HSQUIRRELVM v,SQInteger idx)
{
    v->Push(stack_get(v, idx));
}

/**
 * @brief 指定インデックスのスタック要素の型を取得する。
 * 
 * @details
 * 指定されたスタックインデックス `idx` に存在するオブジェクトの型を取得し、
 * SQObjectType（例：OT_NULL、OT_INTEGER、OT_STRING など）として返す。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 調査対象のスタックインデックス。
 * 
 * @return SQObjectType スタック上のオブジェクトの型。
 */
SQObjectType sq_gettype(HSQUIRRELVM v,SQInteger idx)
{
    return sq_type(stack_get(v, idx));
}

/**
 * @brief 指定されたスタックインデックスのオブジェクトの型名を取得する。
 * 
 * @details
 * スタック上の指定インデックス `idx` に存在するオブジェクトの型を文字列として取得し、
 * その型名をスタックのトップにプッシュする。  
 * 内部的には仮想マシンの `TypeOf` 関数を利用して型を判定し、文字列化している。
 * 
 * @param v 対象の仮想マシン。
 * @param idx 型を取得したいオブジェクトのスタックインデックス。
 * 
 * @retval SQ_OK 成功時。
 * @retval SQ_ERROR 型取得に失敗した場合。
 */
SQRESULT sq_typeof(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &o = stack_get(v, idx);
    SQObjectPtr res;
    if(!v->TypeOf(o,res)) {
        return SQ_ERROR;
    }
    v->Push(res);
    return SQ_OK;
}

/**
 * @brief スタックの指定インデックスにあるオブジェクトを文字列に変換してプッシュする。
 *
 * @details
 * 指定されたインデックス `idx` のオブジェクトを文字列形式に変換し、
 * 成功すればその文字列オブジェクトをスタックにプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタック上の対象オブジェクトのインデックス。
 * @retval SQ_OK 成功した場合。
 * @retval SQ_ERROR 変換に失敗した場合。
 */
SQRESULT sq_tostring(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &o = stack_get(v, idx);
    SQObjectPtr res;
    if(!v->ToString(o,res)) {
        return SQ_ERROR;
    }
    v->Push(res);
    return SQ_OK;
}

/**
 * @brief 指定インデックスのオブジェクトを真偽値として取得する。
 *
 * @details
 * スタックのインデックス `idx` にあるオブジェクトの真偽値を評価し、
 * 結果を `b` に格納する。
 *
 * @param v 対象の仮想マシン。
 * @param idx 評価対象のスタックインデックス。
 * @param[out] b 評価された真偽値の出力先。
 */
void sq_tobool(HSQUIRRELVM v, SQInteger idx, SQBool *b)
{
    SQObjectPtr &o = stack_get(v, idx);
    *b = SQVM::IsFalse(o)?SQFalse:SQTrue;
}

/**
 * @brief スタック上の数値またはブール値を整数として取得する。
 *
 * @details
 * インデックス `idx` のオブジェクトが数値型（整数または浮動小数点数）またはブール値の場合に、
 * 整数として変換し `i` に格納する。どちらでもない場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] i 結果として格納される整数のポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 数値でもブール値でもない場合。
 */
SQRESULT sq_getinteger(HSQUIRRELVM v,SQInteger idx,SQInteger *i)
{
    SQObjectPtr &o = stack_get(v, idx);
    if(sq_isnumeric(o)) {
        *i = tointeger(o);
        return SQ_OK;
    }
    if(sq_isbool(o)) {
        *i = SQVM::IsFalse(o)?SQFalse:SQTrue;
        return SQ_OK;
    }
    return SQ_ERROR;
}

/**
 * @brief スタック上の数値を浮動小数点数として取得する。
 *
 * @details
 * インデックス `idx` にあるオブジェクトが数値型（整数または浮動小数点数）である場合、
 * 浮動小数点数に変換して `f` に格納する。それ以外の型であれば失敗となる。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] f 結果として格納される浮動小数点数のポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 数値でない場合。
 */
SQRESULT sq_getfloat(HSQUIRRELVM v,SQInteger idx,SQFloat *f)
{
    SQObjectPtr &o = stack_get(v, idx);
    if(sq_isnumeric(o)) {
        *f = tofloat(o);
        return SQ_OK;
    }
    return SQ_ERROR;
}

/**
 * @brief スタック上のブール値を取得する。
 *
 * @details
 * スタックのインデックス `idx` にあるオブジェクトがブール値型である場合に、
 * その値を整数型で `b` に格納する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] b 結果として格納されるブール値。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR ブール値でない場合。
 * */
SQRESULT sq_getbool(HSQUIRRELVM v,SQInteger idx,SQBool *b)
{
    SQObjectPtr &o = stack_get(v, idx);
    if(sq_isbool(o)) {
        *b = _integer(o);
        return SQ_OK;
    }
    return SQ_ERROR;
}

/**
 * @brief 指定インデックスの文字列とそのサイズを取得する。
 *
 * @details
 * スタックの指定インデックス `idx` にあるオブジェクトが文字列型の場合、
 * そのポインタと長さを取得して `c` および `size` に格納する。
 * 
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] c 取得された文字列へのポインタ。
 * @param[out] size 文字列の長さ。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getstringandsize(HSQUIRRELVM v,SQInteger idx,const SQChar **c,SQInteger *size)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_STRING,o);
    *c = _stringval(*o);
    *size = _string(*o)->_len;
    return SQ_OK;
}

/**
 * @brief 指定インデックスの文字列を取得する。
 *
 * @details
 * スタックの指定インデックス `idx` にあるオブジェクトが文字列型の場合、
 * そのポインタを `c` に格納する。
 * 文字列長は不要な場合にこの関数を使用する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] c 取得された文字列へのポインタ。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getstring(HSQUIRRELVM v,SQInteger idx,const SQChar **c)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_STRING,o);
    *c = _stringval(*o);
    return SQ_OK;
}

/**
 * @brief 指定インデックスのスレッドオブジェクトを取得する。
 *
 * @details
 * スタックの指定されたインデックスにあるオブジェクトがスレッド型（`OT_THREAD`）であるかを確認し、
 * 該当オブジェクトをスレッドポインタとして `thread` に格納する。
 * スレッドオブジェクトを操作する際に使用される。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] thread 取得されたスレッドオブジェクトへのポインタ。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getthread(HSQUIRRELVM v,SQInteger idx,HSQUIRRELVM *thread)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_THREAD,o);
    *thread = _thread(*o);
    return SQ_OK;
}

/**
 * @brief 指定インデックスのオブジェクトをクローンする。
 *
 * @details
 * スタックの `idx` にあるオブジェクトをクローンし、新しいクローンオブジェクトを
 * スタック上にプッシュする。クローンに失敗した場合はエラーコードを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx クローン対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クローンに失敗した場合。
 */
SQRESULT sq_clone(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &o = stack_get(v,idx);
    v->PushNull();
    if(!v->Clone(o, stack_get(v, -1))){
        v->Pop();
        return SQ_ERROR;
    }
    return SQ_OK;
}

/**
 * @brief 指定インデックスのオブジェクトのサイズを取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトの型に応じて、そのサイズ（文字列長、テーブル要素数、
 * 配列要素数、ユーザーデータサイズなど）を返す。対応していない型の場合はエラー値を返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx サイズを取得するオブジェクトのスタックインデックス。
 * @return SQInteger オブジェクトのサイズ。
 */
SQInteger sq_getsize(HSQUIRRELVM v, SQInteger idx)
{
    SQObjectPtr &o = stack_get(v, idx);
    SQObjectType type = sq_type(o);
    switch(type) {
    case OT_STRING:     return _string(o)->_len;
    case OT_TABLE:      return _table(o)->CountUsed();
    case OT_ARRAY:      return _array(o)->Size();
    case OT_USERDATA:   return _userdata(o)->_size;
    case OT_INSTANCE:   return _instance(o)->_class->_udsize;
    case OT_CLASS:      return _class(o)->_udsize;
    default:
        return sq_aux_invalidtype(v, type);
    }
}

/**
 * @brief 指定インデックスのオブジェクトのハッシュ値を取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトのハッシュ値を計算して返す。
 * ハッシュはオブジェクトの比較やマップのキーとして使用される。
 *
 * @param v 対象の仮想マシン。
 * @param idx ハッシュを取得するオブジェクトのスタックインデックス。
 * @return SQHash 計算されたハッシュ値。
 */
SQHash sq_gethash(HSQUIRRELVM v, SQInteger idx)
{
    SQObjectPtr &o = stack_get(v, idx);
    return HashObj(o);
}

/**
 * @brief 指定インデックスのユーザーデータと型タグを取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがユーザーデータ型である場合、
 * そのポインタを `p` に格納し、型タグがある場合は `typetag` に格納する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] p 取得されたユーザーデータポインタ。
 * @param[out] typetag 型タグを受け取るポインタ（省略可）。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getuserdata(HSQUIRRELVM v,SQInteger idx,SQUserPointer *p,SQUserPointer *typetag)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_USERDATA,o);
    (*p) = _userdataval(*o);
    if(typetag) *typetag = _userdata(*o)->_typetag;
    return SQ_OK;
}

/**
 * @brief 指定インデックスのオブジェクトに型タグを設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがユーザーデータまたはクラス型の場合、
 * 与えられた型タグを設定する。それ以外の型の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param typetag 設定する型タグ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がユーザーデータでもクラスでもない場合。
 */
SQRESULT sq_settypetag(HSQUIRRELVM v,SQInteger idx,SQUserPointer typetag)
{
    SQObjectPtr &o = stack_get(v,idx);
    switch(sq_type(o)) {
        case OT_USERDATA:   _userdata(o)->_typetag = typetag;   break;
        case OT_CLASS:      _class(o)->_typetag = typetag;      break;
        default:            return sq_throwerror(v,_SC("invalid object type"));
    }
    return SQ_OK;
}

/**
 * @brief オブジェクトの型タグを取得する。
 *
 * @details
 * 与えられた `HSQOBJECT` がインスタンス、ユーザーデータ、またはクラス型の場合、
 * その型タグを `typetag` に格納する。それ以外の型の場合はエラーを返す。
 *
 * @param o 対象のオブジェクトポインタ。
 * @param[out] typetag 取得した型タグを格納するポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対象がサポートされない型の場合。
 */
SQRESULT sq_getobjtypetag(const HSQOBJECT *o,SQUserPointer * typetag)
{
  switch(sq_type(*o)) {
    case OT_INSTANCE: *typetag = _instance(*o)->_class->_typetag; break;
    case OT_USERDATA: *typetag = _userdata(*o)->_typetag; break;
    case OT_CLASS:    *typetag = _class(*o)->_typetag; break;
    default: return SQ_ERROR;
  }
  return SQ_OK;
}

/**
 * @brief スタックインデックスのオブジェクトに設定された型タグを取得する。
 *
 * @details
 * 指定されたスタックインデックスのオブジェクトの型タグを取得し、
 * `typetag` に格納する。内部的には `sq_getobjtypetag` を呼び出して処理する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] typetag 取得された型タグを格納するポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 型タグを取得できなかった場合。
 */
SQRESULT sq_gettypetag(HSQUIRRELVM v,SQInteger idx,SQUserPointer *typetag)
{
    SQObjectPtr &o = stack_get(v,idx);
    if (SQ_FAILED(sq_getobjtypetag(&o, typetag)))
        return SQ_ERROR;// this is not an error it should be a bool but would break backward compatibility
    return SQ_OK;
}

/**
 * @brief スタックインデックスのユーザーポインタを取得する。
 *
 * @details
 * 指定されたスタックインデックス `idx` にあるオブジェクトがユーザーポインタ型の場合、
 * その値を `p` に格納する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] p 取得されたユーザーポインタ。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getuserpointer(HSQUIRRELVM v, SQInteger idx, SQUserPointer *p)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_USERPOINTER,o);
    (*p) = _userpointer(*o);
    return SQ_OK;
}

/**
 * @brief インスタンスにユーザーポインタを設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがクラスインスタンス型である場合、
 * そのインスタンスに対して `p` で指定されたユーザーポインタを紐付ける。
 * クラスインスタンス以外の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象インスタンスのスタックインデックス。
 * @param p 設定するユーザーポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クラスインスタンスでない場合。
 */
SQRESULT sq_setinstanceup(HSQUIRRELVM v, SQInteger idx, SQUserPointer p)
{
    SQObjectPtr &o = stack_get(v,idx);
    if(sq_type(o) != OT_INSTANCE) return sq_throwerror(v,_SC("the object is not a class instance"));
    _instance(o)->_userpointer = p;
    return SQ_OK;
}

/**
 * @brief クラスのユーザーデータサイズを設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがクラス型である場合に、
 * そのクラスのユーザーデータ領域のサイズを `udsize` に設定する。
 * ロックされたクラスには設定できない。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @param udsize 設定するユーザーデータサイズ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クラス型でない場合、またはクラスがロックされている場合。
 */
SQRESULT sq_setclassudsize(HSQUIRRELVM v, SQInteger idx, SQInteger udsize)
{
    SQObjectPtr &o = stack_get(v,idx);
    if(sq_type(o) != OT_CLASS) return sq_throwerror(v,_SC("the object is not a class"));
    if(_class(o)->_locked) return sq_throwerror(v,_SC("the class is locked"));
    _class(o)->_udsize = udsize;
    return SQ_OK;
}

/**
 * @brief クラスインスタンスのユーザーポインタを取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがクラスインスタンス型である場合、
 * そのユーザーポインタを `p` に取得する。`typetag` を指定すると、
 * インスタンスが指定された型タグを持っているかを確認する。型タグが一致しない場合や、
 * オブジェクトがインスタンスでない場合にはエラーを返す（throwerror が true の場合）。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] p 取得されたユーザーポインタ。
 * @param typetag 確認する型タグ（省略可）。
 * @param throwerror 型タグが一致しない場合にエラーを投げるかどうか。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_getinstanceup(HSQUIRRELVM v, SQInteger idx, SQUserPointer *p, SQUserPointer typetag, SQBool throwerror)
{
	SQObjectPtr &o = stack_get(v, idx);
	if (sq_type(o) != OT_INSTANCE) return throwerror ? sq_throwerror(v, _SC("the object is not a class instance")) : SQ_ERROR;
	(*p) = _instance(o)->_userpointer;
	if (typetag != 0) {
		SQClass *cl = _instance(o)->_class;
		do {
			if (cl->_typetag == typetag)
				return SQ_OK;
			cl = cl->_base;
		} while (cl != NULL);
		return throwerror ? sq_throwerror(v, _SC("invalid type tag")) : SQ_ERROR;
	}
	return SQ_OK;
}

/**
 * @brief スタック上の現在のトップインデックスを取得する。
 *
 * @details
 * 現在のスタックトップの位置を返す。この値はスタックベースからの相対値であり、
 * スタック操作の状態を確認する際に使用する。
 *
 * @param v 対象の仮想マシン。
 * @return SQInteger スタックトップのインデックス。
 */
SQInteger sq_gettop(HSQUIRRELVM v)
{
    return (v->_top) - v->_stackbase;
}

/**
 * @brief スタックのトップを新しい位置に設定する。
 *
 * @details
 * スタックの現在のトップを `newtop` に設定する。現在のトップが新しい値より大きい場合、
 * 超過分の要素をポップする。逆に小さい場合は `null` をプッシュして補う。
 *
 * @param v 対象の仮想マシン。
 * @param newtop 新しく設定するスタックトップのインデックス。
 */
void sq_settop(HSQUIRRELVM v, SQInteger newtop)
{
    SQInteger top = sq_gettop(v);
    if(top > newtop)
        sq_pop(v, top - newtop);
    else
        while(top++ < newtop) sq_pushnull(v);
}

/**
 * @brief スタックのトップから指定数の要素をポップする。
 *
 * @details
 * スタックの現在のトップから `nelemstopop` 個の要素を削除する。スタックの
 * 要素数が指定数未満の場合は内部でアサートが発生する。
 *
 * @param v 対象の仮想マシン。
 * @param nelemstopop ポップする要素の個数。
 */
void sq_pop(HSQUIRRELVM v, SQInteger nelemstopop)
{
    assert(v->_top >= nelemstopop);
    v->Pop(nelemstopop);
}

/**
 * @brief スタックのトップ要素を1つポップする。
 *
 * @details
 * スタックのトップにある要素を1つだけ削除する。スタックが空の場合は
 * 内部でアサートが発生する。
 *
 * @param v 対象の仮想マシン。
 */
void sq_poptop(HSQUIRRELVM v)
{
    assert(v->_top >= 1);
    v->Pop();
}

/**
 * @brief スタックの指定インデックスの要素を削除する。
 *
 * @details
 * スタック内の指定されたインデックス `idx` にある要素を削除し、
 * それ以降の要素を詰めて順序を維持する。
 *
 * @param v 対象の仮想マシン。
 * @param idx 削除する要素のスタックインデックス。
 */
void sq_remove(HSQUIRRELVM v, SQInteger idx)
{
    v->Remove(idx);
}

/**
 * @brief スタックトップ2つのオブジェクトを比較する。
 *
 * @details
 * スタックのトップにある2つのオブジェクトを比較し、比較結果を整数として返す。
 *
 * @param v 対象の仮想マシン。
 * @return SQInteger 比較結果（-1, 0, 1）。
 */
SQInteger sq_cmp(HSQUIRRELVM v)
{
    SQInteger res;
    v->ObjCmp(stack_get(v, -1), stack_get(v, -2),res);
    return res;
}

/**
 * @brief テーブルまたはクラスに新しいスロット（キーと値のペア）を追加する。
 *
 * @details
 * スタックの指定インデックス `idx` にあるテーブルまたはクラスに対して、
 * スタックトップ2つの要素（キーと値）を使用して新しいスロットを追加する。
 * スロットを静的メンバーとして追加することも可能。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象テーブルまたはクラスのスタックインデックス。
 * @param bstatic スロットを静的にする場合 true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR キーが null の場合などのエラー。
 */
SQRESULT sq_newslot(HSQUIRRELVM v, SQInteger idx, SQBool bstatic)
{
    sq_aux_paramscheck(v, 3);
    SQObjectPtr &self = stack_get(v, idx);
    if(sq_type(self) == OT_TABLE || sq_type(self) == OT_CLASS) {
        SQObjectPtr &key = v->GetUp(-2);
        if(sq_type(key) == OT_NULL) return sq_throwerror(v, _SC("null is not a valid key"));
        v->NewSlot(self, key, v->GetUp(-1),bstatic?true:false);
        v->Pop(2);
    }
    return SQ_OK;
}

/**
 * @brief テーブルのスロット（キーと値のペア）を削除する。
 *
 * @details
 * 指定インデックス `idx` にあるテーブルからスタックトップにあるキーで指定されるスロットを削除する。
 * 削除後に値をスタックにプッシュするかどうかは `pushval` で制御する。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象テーブルのスタックインデックス。
 * @param pushval 削除した値をプッシュする場合 true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR キーが無効な場合などのエラー。
 */
SQRESULT sq_deleteslot(HSQUIRRELVM v,SQInteger idx,SQBool pushval)
{
    sq_aux_paramscheck(v, 2);
    SQObjectPtr *self;
    _GETSAFE_OBJ(v, idx, OT_TABLE,self);
    SQObjectPtr &key = v->GetUp(-1);
    if(sq_type(key) == OT_NULL) return sq_throwerror(v, _SC("null is not a valid key"));
    SQObjectPtr res;
    if(!v->DeleteSlot(*self, key, res)){
        v->Pop();
        return SQ_ERROR;
    }
    if(pushval) v->GetUp(-1) = res;
    else v->Pop();
    return SQ_OK;
}

/**
 * @brief テーブルまたはクラスのスロットを設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクト（テーブルやクラスなど）に対して、
 * スタックトップのキーと値を使用してスロットを設定する。
 * キーが null の場合などはエラーになる。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_set(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &self = stack_get(v, idx);
    if(v->Set(self, v->GetUp(-2), v->GetUp(-1),DONT_FALL_BACK)) {
        v->Pop(2);
        return SQ_OK;
    }
    return SQ_ERROR;
}

/**
 * @brief テーブル・クラス・インスタンスなどのスロットを生で設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトに対して、スタックトップのキーと値を
 * 生で設定する。型に応じて適切なスロット追加を行う。対応しない型の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗または無効なキー。
 */
SQRESULT sq_rawset(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &self = stack_get(v, idx);
    SQObjectPtr &key = v->GetUp(-2);
    if(sq_type(key) == OT_NULL) {
        v->Pop(2);
        return sq_throwerror(v, _SC("null key"));
    }
    switch(sq_type(self)) {
    case OT_TABLE:
        _table(self)->NewSlot(key, v->GetUp(-1));
        v->Pop(2);
        return SQ_OK;
    break;
    case OT_CLASS:
        _class(self)->NewSlot(_ss(v), key, v->GetUp(-1),false);
        v->Pop(2);
        return SQ_OK;
    break;
    case OT_INSTANCE:
        if(_instance(self)->Set(key, v->GetUp(-1))) {
            v->Pop(2);
            return SQ_OK;
        }
    break;
    case OT_ARRAY:
        if(v->Set(self, key, v->GetUp(-1),false)) {
            v->Pop(2);
            return SQ_OK;
        }
    break;
    default:
        v->Pop(2);
        return sq_throwerror(v, _SC("rawset works only on array/table/class and instance"));
    }
    v->Raise_IdxError(v->GetUp(-2));return SQ_ERROR;
}

/**
 * @brief クラスに新しいメンバーを追加する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがクラス型の場合に、スタックトップのキー・値・属性を用いて
 * 新しいメンバーを追加する。メンバーを静的にするかどうかは `bstatic` で制御する。
 * クラス型以外の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @param bstatic 静的メンバーとして追加する場合 true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クラス型でない場合、またはキーが null の場合。
 */
SQRESULT sq_newmember(HSQUIRRELVM v,SQInteger idx,SQBool bstatic)
{
    SQObjectPtr &self = stack_get(v, idx);
    if(sq_type(self) != OT_CLASS) return sq_throwerror(v, _SC("new member only works with classes"));
    SQObjectPtr &key = v->GetUp(-3);
    if(sq_type(key) == OT_NULL) return sq_throwerror(v, _SC("null key"));
    if(!v->NewSlotA(self,key,v->GetUp(-2),v->GetUp(-1),bstatic?true:false,false)) {
        v->Pop(3);
        return SQ_ERROR;
    }
    v->Pop(3);
    return SQ_OK;
}

/**
 * @brief クラスに新しいメンバーを生で追加する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがクラス型の場合に、スタックトップのキー・値・属性を用いて
 * 新しいメンバーを生で追加する（基底クラスのスロットに直接追加）。
 * メンバーを静的にするかどうかは `bstatic` で制御する。
 * クラス型以外の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @param bstatic 静的メンバーとして追加する場合 true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クラス型でない場合、またはキーが null の場合。
 */
SQRESULT sq_rawnewmember(HSQUIRRELVM v,SQInteger idx,SQBool bstatic)
{
    SQObjectPtr &self = stack_get(v, idx);
    if(sq_type(self) != OT_CLASS) return sq_throwerror(v, _SC("new member only works with classes"));
    SQObjectPtr &key = v->GetUp(-3);
    if(sq_type(key) == OT_NULL) return sq_throwerror(v, _SC("null key"));
    if(!v->NewSlotA(self,key,v->GetUp(-2),v->GetUp(-1),bstatic?true:false,true)) {
        v->Pop(3);
        return SQ_ERROR;
    }
    v->Pop(3);
    return SQ_OK;
}

/**
 * @brief テーブルまたはユーザーデータにデリゲートを設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがテーブルまたはユーザーデータ型の場合、
 * スタックトップのオブジェクトをデリゲートとして設定する。無効な型の場合や、
 * デリゲートがサイクルする場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効な型やサイクルが検出された場合。
 */
SQRESULT sq_setdelegate(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &self = stack_get(v, idx);
    SQObjectPtr &mt = v->GetUp(-1);
    SQObjectType type = sq_type(self);
    switch(type) {
    case OT_TABLE:
        if(sq_type(mt) == OT_TABLE) {
            if(!_table(self)->SetDelegate(_table(mt))) {
                return sq_throwerror(v, _SC("delegate cycle"));
            }
            v->Pop();
        }
        else if(sq_type(mt)==OT_NULL) {
            _table(self)->SetDelegate(NULL); v->Pop(); }
        else return sq_aux_invalidtype(v,type);
        break;
    case OT_USERDATA:
        if(sq_type(mt)==OT_TABLE) {
            _userdata(self)->SetDelegate(_table(mt)); v->Pop(); }
        else if(sq_type(mt)==OT_NULL) {
            _userdata(self)->SetDelegate(NULL); v->Pop(); }
        else return sq_aux_invalidtype(v, type);
        break;
    default:
            return sq_aux_invalidtype(v, type);
        break;
    }
    return SQ_OK;
}

/**
 * @brief テーブルのスロットを生で削除する。
 *
 * @details
 * スタックの `idx` にあるテーブルから、スタックトップのキーに対応する
 * スロットを削除する。削除した値を残すかどうかは `pushval` で制御する。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象テーブルのスタックインデックス。
 * @param pushval 削除した値をプッシュする場合 true。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_rawdeleteslot(HSQUIRRELVM v,SQInteger idx,SQBool pushval)
{
    sq_aux_paramscheck(v, 2);
    SQObjectPtr *self;
    _GETSAFE_OBJ(v, idx, OT_TABLE,self);
    SQObjectPtr &key = v->GetUp(-1);
    SQObjectPtr t;
    if(_table(*self)->Get(key,t)) {
        _table(*self)->Remove(key);
    }
    if(pushval != 0)
        v->GetUp(-1) = t;
    else
        v->Pop();
    return SQ_OK;
}

/**
 * @brief テーブルまたはユーザーデータのデリゲートを取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがテーブルまたはユーザーデータ型の場合、
 * そのデリゲートをスタックにプッシュする。デリゲートが存在しない場合は null をプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効な型の場合。
 */
SQRESULT sq_getdelegate(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &self=stack_get(v,idx);
    switch(sq_type(self)){
    case OT_TABLE:
    case OT_USERDATA:
        if(!_delegable(self)->_delegate){
            v->PushNull();
            break;
        }
        v->Push(SQObjectPtr(_delegable(self)->_delegate));
        break;
    default: return sq_throwerror(v,_SC("wrong type")); break;
    }
    return SQ_OK;

}

/**
 * @brief テーブル・クラス・インスタンスなどから値を取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトから、スタックトップにあるキーを使って値を取得する。
 * キーが存在しない場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR キーが存在しない場合など。
 */
SQRESULT sq_get(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &self=stack_get(v,idx);
    SQObjectPtr &obj = v->GetUp(-1);
    if(v->Get(self,obj,obj,false,DONT_FALL_BACK))
        return SQ_OK;
    v->Pop();
    return SQ_ERROR;
}

/**
 * @brief テーブル・クラス・インスタンスなどから値を生で取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトから、スタックトップのキーを使って値を取得する。
 * この関数はフォールバックを行わない低レベルの取得を行う。無効な型やインデックスの場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_rawget(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &self=stack_get(v,idx);
    SQObjectPtr &obj = v->GetUp(-1);
    switch(sq_type(self)) {
    case OT_TABLE:
        if(_table(self)->Get(obj,obj))
            return SQ_OK;
        break;
    case OT_CLASS:
        if(_class(self)->Get(obj,obj))
            return SQ_OK;
        break;
    case OT_INSTANCE:
        if(_instance(self)->Get(obj,obj))
            return SQ_OK;
        break;
    case OT_ARRAY:{
        if(sq_isnumeric(obj)){
            if(_array(self)->Get(tointeger(obj),obj)) {
                return SQ_OK;
            }
        }
        else {
            v->Pop();
            return sq_throwerror(v,_SC("invalid index type for an array"));
        }
                  }
        break;
    default:
        v->Pop();
        return sq_throwerror(v,_SC("rawget works only on array/table/instance and class"));
    }
    v->Pop();
    return sq_throwerror(v,_SC("the index doesn't exist"));
}

/**
 * @brief スタックインデックスのオブジェクトを取得する。
 *
 * @details
 * 指定されたスタックインデックスのオブジェクトを `HSQOBJECT` として取得し、
 * `po` に格納する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param[out] po 取得されたオブジェクトを格納するポインタ。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getstackobj(HSQUIRRELVM v,SQInteger idx,HSQOBJECT *po)
{
    *po=stack_get(v,idx);
    return SQ_OK;
}

/**
 * @brief 指定レベルのローカル変数の名前を取得する。
 *
 * @details
 * 指定されたコールスタックレベル `level` とローカル変数のインデックス `idx` に対応する
 * ローカル変数の名前を返す。該当するローカル変数が存在しない場合は NULL を返す。
 *
 * @param v 対象の仮想マシン。
 * @param level コールスタックレベル。
 * @param idx ローカル変数のインデックス。
 * @return const SQChar* ローカル変数の名前、存在しない場合は NULL。
 */
const SQChar *sq_getlocal(HSQUIRRELVM v,SQUnsignedInteger level,SQUnsignedInteger idx)
{
    SQUnsignedInteger cstksize=v->_callsstacksize;
    SQUnsignedInteger lvl=(cstksize-level)-1;
    SQInteger stackbase=v->_stackbase;
    if(lvl<cstksize){
        for(SQUnsignedInteger i=0;i<level;i++){
            SQVM::CallInfo &ci=v->_callsstack[(cstksize-i)-1];
            stackbase-=ci._prevstkbase;
        }
        SQVM::CallInfo &ci=v->_callsstack[lvl];
        if(sq_type(ci._closure)!=OT_CLOSURE)
            return NULL;
        SQClosure *c=_closure(ci._closure);
        SQFunctionProto *func=c->_function;
        if(func->_noutervalues > (SQInteger)idx) {
            v->Push(*_outer(c->_outervalues[idx])->_valptr);
            return _stringval(func->_outervalues[idx]._name);
        }
        idx -= func->_noutervalues;
        return func->GetLocal(v,stackbase,idx,(SQInteger)(ci._ip-func->_instructions)-1);
    }
    return NULL;
}

/**
 * @brief オブジェクトをスタックにプッシュする。
 *
 * @details
 * 指定された `HSQOBJECT` 型のオブジェクトを現在の VM のスタックにプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param obj プッシュするオブジェクト。
 */
void sq_pushobject(HSQUIRRELVM v,HSQOBJECT obj)
{
    v->Push(SQObjectPtr(obj));
}

/**
 * @brief オブジェクトを初期化状態にリセットする。
 *
 * @details
 * 指定された `HSQOBJECT` を `NULL` 型に初期化し、ユーザーポインタをクリアする。
 * オブジェクトの再利用時に使用する。
 *
 * @param po リセットするオブジェクトポインタ。
 */
void sq_resetobject(HSQOBJECT *po)
{
    po->_unVal.pUserPointer=NULL;
    po->_type=OT_NULL;
}

/**
 * @brief エラーメッセージを設定してエラーを発生させる。
 *
 * @details
 * 指定された文字列 `err` を VM のラストエラーとして設定し、
 * SQ_ERROR を返す。この関数はスクリプト実行中のエラー通知に使用する。
 *
 * @param v 対象の仮想マシン。
 * @param err 設定するエラーメッセージ。
 * @retval SQ_ERROR 常にエラーを返す。
 */
SQRESULT sq_throwerror(HSQUIRRELVM v,const SQChar *err)
{
    v->_lasterror=SQString::Create(_ss(v),err);
    return SQ_ERROR;
}

/**
 * @brief スタックトップのオブジェクトをエラーとして投げる。
 *
 * @details
 * スタックトップにあるオブジェクトをラストエラーとして設定し、
 * SQ_ERROR を返す。カスタムオブジェクトをエラーとして返したい場合に使用する。
 *
 * @param v 対象の仮想マシン。
 * @retval SQ_ERROR 常にエラーを返す。
 */
SQRESULT sq_throwobject(HSQUIRRELVM v)
{
    v->_lasterror = v->GetUp(-1);
    v->Pop();
    return SQ_ERROR;
}

/**
 * @brief ラストエラーをリセットする。
 *
 * @details
 * 仮想マシンに設定されているラストエラーを NULL に初期化する。
 * エラー処理後にエラー状態をクリアしたい場合に使用する。
 *
 * @param v 対象の仮想マシン。
 */
void sq_reseterror(HSQUIRRELVM v)
{
    v->_lasterror.Null();
}

/**
 * @brief ラストエラーをスタックにプッシュする。
 *
 * @details
 * 仮想マシンに現在設定されているラストエラーオブジェクトを
 * スタックにプッシュする。エラー内容を取得して処理したい場合に使用する。
 *
 * @param v 対象の仮想マシン。
 */
void sq_getlasterror(HSQUIRRELVM v)
{
    v->Push(v->_lasterror);
}

/**
 * @brief スタックサイズを予約する。
 *
 * @details
 * スタックの必要サイズ `nsize` を予約する。現在のスタックサイズが不足している場合、
 * メタメソッドの実行中でない限りスタックを拡張する。メタメソッド実行中の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param nsize 予約したいスタックサイズ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR メタメソッド実行中の場合。
 */
SQRESULT sq_reservestack(HSQUIRRELVM v,SQInteger nsize)
{
    if (((SQUnsignedInteger)v->_top + nsize) > v->_stack.size()) {
        if(v->_nmetamethodscall) {
            return sq_throwerror(v,_SC("cannot resize stack while in a metamethod"));
        }
        v->_stack.resize(v->_stack.size() + ((v->_top + nsize) - v->_stack.size()));
    }
    return SQ_OK;
}

/**
 * @brief サスペンド状態のジェネレータを再開する。
 *
 * @details
 * スタックトップにあるジェネレータを再開して実行を継続する。対象がジェネレータ以外の場合はエラー。
 *
 * @param v 対象の仮想マシン。
 * @param retval 戻り値を残す場合は true。
 * @param raiseerror エラー発生時に例外を送出する場合は true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR ジェネレータ以外の場合や失敗時。
 */
SQRESULT sq_resume(HSQUIRRELVM v,SQBool retval,SQBool raiseerror)
{
    if (sq_type(v->GetUp(-1)) == OT_GENERATOR)
    {
        v->PushNull(); //retval
        if (!v->Execute(v->GetUp(-2), 0, v->_top, v->GetUp(-1), raiseerror, SQVM::ET_RESUME_GENERATOR))
        {v->Raise_Error(v->_lasterror); return SQ_ERROR;}
        if(!retval)
            v->Pop();
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("only generators can be resumed"));
}

/**
 * @brief 関数またはクロージャを呼び出す。
 *
 * @details
 * スタックトップにある関数（クロージャ）を、指定した引数の数 `params` を用いて呼び出す。
 * 呼び出し結果をスタックに残すかは `retval` で制御する。呼び出し時にエラーが発生した場合、
 * `raiseerror` が true なら例外を送出する。
 *
 * @param v 対象の仮想マシン。
 * @param params 引数の個数。
 * @param retval 戻り値を残す場合 true。
 * @param raiseerror エラー発生時に例外を送出する場合 true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 呼び出し失敗時。
 */
SQRESULT sq_call(HSQUIRRELVM v,SQInteger params,SQBool retval,SQBool raiseerror)
{
    SQObjectPtr res;
    if(!v->Call(v->GetUp(-(params+1)),params,v->_top-params,res,raiseerror?true:false)){
        v->Pop(params); //pop args
        return SQ_ERROR;
    }
    if(!v->_suspended)
        v->Pop(params); //pop args
    if(retval)
        v->Push(res); // push result
    return SQ_OK;
}

/**
 * @brief 関数またはクロージャを末尾呼び出しする。
 *
 * @details
 * スタックトップにある関数（クロージャ）を末尾呼び出しとして実行する。
 * ジェネレータの末尾呼び出しはできない。クロージャ以外のオブジェクトの場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param nparams 引数の個数。
 * @retval SQ_TAILCALL_FLAG 成功した場合の末尾呼び出しフラグ。
 * @retval SQ_ERROR 失敗時。
 */
SQRESULT sq_tailcall(HSQUIRRELVM v, SQInteger nparams)
{
	SQObjectPtr &res = v->GetUp(-(nparams + 1));
	if (sq_type(res) != OT_CLOSURE) {
		return sq_throwerror(v, _SC("only closure can be tail called"));
	}
	SQClosure *clo = _closure(res);
	if (clo->_function->_bgenerator)
	{
		return sq_throwerror(v, _SC("generators cannot be tail called"));
	}
	
	SQInteger stackbase = (v->_top - nparams) - v->_stackbase;
	if (!v->TailCall(clo, stackbase, nparams)) {
		return SQ_ERROR;
	}
	return SQ_TAILCALL_FLAG;
}

/**
 * @brief 仮想マシンをサスペンド状態にする。
 *
 * @details
 * 実行中の仮想マシンをサスペンド状態にして、後で再開できるようにする。
 * コルーチンやジェネレータの制御フローに使用される。
 *
 * @param v 対象の仮想マシン。
 * @retval SQ_OK サスペンド成功時。
 */
SQRESULT sq_suspendvm(HSQUIRRELVM v)
{
    return v->Suspend();
}

/**
 * @brief サスペンド状態の仮想マシンを再開する。
 *
 * @details
 * サスペンド状態にある仮想マシンを再開する。戻り値を設定したり、例外を送出するかどうかなどを
 * オプションで制御できる。再開時に対象がサスペンド状態でない場合はエラーとなる。
 *
 * @param v 対象の仮想マシン。
 * @param wakeupret 戻り値を設定する場合 true。
 * @param retval 戻り値をスタックにプッシュする場合 true。
 * @param raiseerror エラー時に例外を送出する場合 true。
 * @param throwerror VM内で例外を投げる場合 true。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_wakeupvm(HSQUIRRELVM v,SQBool wakeupret,SQBool retval,SQBool raiseerror,SQBool throwerror)
{
    SQObjectPtr ret;
    if(!v->_suspended)
        return sq_throwerror(v,_SC("cannot resume a vm that is not running any code"));
    SQInteger target = v->_suspended_target;
    if(wakeupret) {
        if(target != -1) {
            v->GetAt(v->_stackbase+v->_suspended_target)=v->GetUp(-1); //retval
        }
        v->Pop();
    } else if(target != -1) { v->GetAt(v->_stackbase+v->_suspended_target).Null(); }
    SQObjectPtr dummy;
    if(!v->Execute(dummy,-1,-1,ret,raiseerror,throwerror?SQVM::ET_RESUME_THROW_VM : SQVM::ET_RESUME_VM)) {
        return SQ_ERROR;
    }
    if(retval)
        v->Push(ret);
    return SQ_OK;
}

/**
 * @brief オブジェクトにリリースフックを設定する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがユーザーデータ・インスタンス・クラス型の場合、
 * 指定されたリリースフック関数を登録する。他の型では何も行わない。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param hook 設定するリリースフック関数。
 */
void sq_setreleasehook(HSQUIRRELVM v,SQInteger idx,SQRELEASEHOOK hook)
{
    SQObjectPtr &ud=stack_get(v,idx);
    switch(sq_type(ud) ) {
    case OT_USERDATA:   _userdata(ud)->_hook = hook;    break;
    case OT_INSTANCE:   _instance(ud)->_hook = hook;    break;
    case OT_CLASS:      _class(ud)->_hook = hook;       break;
    default: return;
    }
}

/**
 * @brief オブジェクトのリリースフックを取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトがユーザーデータ・インスタンス・クラス型の場合、
 * 設定されているリリースフック関数を取得して返す。他の型の場合は NULL を返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return SQRELEASEHOOK 設定されているリリースフック、または NULL。
 */
SQRELEASEHOOK sq_getreleasehook(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &ud=stack_get(v,idx);
    switch(sq_type(ud) ) {
    case OT_USERDATA:   return _userdata(ud)->_hook;    break;
    case OT_INSTANCE:   return _instance(ud)->_hook;    break;
    case OT_CLASS:      return _class(ud)->_hook;       break;
    default: return NULL;
    }
}

/**
 * @brief コンパイラエラーハンドラを設定する。
 *
 * @details
 * 指定されたコンパイラエラーハンドラ関数を仮想マシンに設定する。
 * スクリプトのコンパイル時に発生するエラーをカスタム処理したい場合に使用する。
 *
 * @param v 対象の仮想マシン。
 * @param f 設定するコンパイラエラーハンドラ関数。
 */
void sq_setcompilererrorhandler(HSQUIRRELVM v,SQCOMPILERERROR f)
{
    _ss(v)->_compilererrorhandler = f;
}

/**
 * @brief クロージャをストリームに書き出す。
 *
 * @details
 * スタックトップにあるクロージャをバイトコードとして書き出す。自由変数がバインドされている場合はエラーを返す。
 * 書き出しには指定された書き込み関数 `w` とユーザーポインタ `up` が使用される。
 *
 * @param v 対象の仮想マシン。
 * @param w 書き込み関数。
 * @param up ユーザーポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR IOエラーまたは自由変数がバインドされている場合。
 */
SQRESULT sq_writeclosure(HSQUIRRELVM v,SQWRITEFUNC w,SQUserPointer up)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, -1, OT_CLOSURE,o);
    unsigned short tag = SQ_BYTECODE_STREAM_TAG;
    if(_closure(*o)->_function->_noutervalues)
        return sq_throwerror(v,_SC("a closure with free variables bound cannot be serialized"));
    if(w(up,&tag,2) != 2)
        return sq_throwerror(v,_SC("io error"));
    if(!_closure(*o)->Save(v,up,w))
        return SQ_ERROR;
    return SQ_OK;
}

/**
 * @brief バイトコードストリームからクロージャを読み込む。
 *
 * @details
 * バイトコードストリームからクロージャを復元して、スタックにプッシュする。
 * ストリームのタグが不正または読み込みエラーが発生した場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param r 読み込み関数。
 * @param up ユーザーポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR IOエラーまたはストリームタグ不正。
 */
SQRESULT sq_readclosure(HSQUIRRELVM v,SQREADFUNC r,SQUserPointer up)
{
    SQObjectPtr closure;

    unsigned short tag;
    if(r(up,&tag,2) != 2)
        return sq_throwerror(v,_SC("io error"));
    if(tag != SQ_BYTECODE_STREAM_TAG)
        return sq_throwerror(v,_SC("invalid stream"));
    if(!SQClosure::Load(v,up,r,closure))
        return SQ_ERROR;
    v->Push(closure);
    return SQ_OK;
}

/**
 * @brief スクラッチパッドを取得する。
 *
 * @details
 * 仮想マシンの内部スクラッチパッドメモリを取得する。メモリサイズは `minsize` 以上になるように確保される。
 * スクラッチパッドは一時的な文字列操作などに利用される。
 *
 * @param v 対象の仮想マシン。
 * @param minsize 必要な最小サイズ。
 * @return SQChar* 確保されたスクラッチパッドのポインタ。
 */
SQChar *sq_getscratchpad(HSQUIRRELVM v,SQInteger minsize)
{
    return _ss(v)->GetScratchPad(minsize);
}

/**
 * @brief 到達不能オブジェクトを復活させる。
 *
 * @details
 * ガーベジコレクタによって到達不能と判定されたオブジェクトを復活させる。
 * ビルドにガーベジコレクタが含まれていない場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR ガーベジコレクタ非対応ビルドの場合。
 */
SQRESULT sq_resurrectunreachable(HSQUIRRELVM v)
{
#ifndef NO_GARBAGE_COLLECTOR
    _ss(v)->ResurrectUnreachable(v);
    return SQ_OK;
#else
    return sq_throwerror(v,_SC("sq_resurrectunreachable requires a garbage collector build"));
#endif
}

/**
 * @brief ガーベジコレクタを実行し、不要なメモリを解放する。
 *
 * @details
 * 仮想マシンのガーベジコレクタを手動で呼び出し、到達不能なオブジェクトを解放する。
 * ガーベジコレクタが無効なビルドの場合は -1 を返す。
 *
 * @param v 対象の仮想マシン。
 * @return SQInteger 解放されたオブジェクト数、または -1。
 */
SQInteger sq_collectgarbage(HSQUIRRELVM v)
{
#ifndef NO_GARBAGE_COLLECTOR
    return _ss(v)->CollectGarbage(v);
#else
    return -1;
#endif
}

/**
 * @brief 現在実行中の呼び出しクロージャを取得する。
 *
 * @details
 * 呼び出しスタックに存在するクロージャを取得し、スタックにプッシュする。
 * 呼び出しスタックにクロージャが存在しない場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR クロージャが存在しない場合。
 */
SQRESULT sq_getcallee(HSQUIRRELVM v)
{
    if(v->_callsstacksize > 1)
    {
        v->Push(v->_callsstack[v->_callsstacksize - 2]._closure);
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("no closure in the calls stack"));
}

/**
 * @brief クロージャの自由変数の名前を取得する。
 *
 * @details
 * スタックの `idx` にあるクロージャまたはネイティブクロージャの指定された
 * インデックスの自由変数の名前を取得する。存在すればその変数の値をプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param nval 取得する自由変数のインデックス。
 * @return const SQChar* 変数名、存在しない場合は NULL。
 */
const SQChar *sq_getfreevariable(HSQUIRRELVM v,SQInteger idx,SQUnsignedInteger nval)
{
    SQObjectPtr &self=stack_get(v,idx);
    const SQChar *name = NULL;
    switch(sq_type(self))
    {
    case OT_CLOSURE:{
        SQClosure *clo = _closure(self);
        SQFunctionProto *fp = clo->_function;
        if(((SQUnsignedInteger)fp->_noutervalues) > nval) {
            v->Push(*(_outer(clo->_outervalues[nval])->_valptr));
            SQOuterVar &ov = fp->_outervalues[nval];
            name = _stringval(ov._name);
        }
                    }
        break;
    case OT_NATIVECLOSURE:{
        SQNativeClosure *clo = _nativeclosure(self);
        if(clo->_noutervalues > nval) {
            v->Push(clo->_outervalues[nval]);
            name = _SC("@NATIVE");
        }
                          }
        break;
    default: break; //shutup compiler
    }
    return name;
}

/**
 * @brief クロージャの自由変数を設定する。
 *
 * @details
 * スタックの `idx` にあるクロージャまたはネイティブクロージャの指定されたインデックスの
 * 自由変数に、スタックトップの値を設定する。インデックスが無効な場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param nval 設定する自由変数のインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効なインデックスや型の場合。
 */
SQRESULT sq_setfreevariable(HSQUIRRELVM v,SQInteger idx,SQUnsignedInteger nval)
{
    SQObjectPtr &self=stack_get(v,idx);
    switch(sq_type(self))
    {
    case OT_CLOSURE:{
        SQFunctionProto *fp = _closure(self)->_function;
        if(((SQUnsignedInteger)fp->_noutervalues) > nval){
            *(_outer(_closure(self)->_outervalues[nval])->_valptr) = stack_get(v,-1);
        }
        else return sq_throwerror(v,_SC("invalid free var index"));
                    }
        break;
    case OT_NATIVECLOSURE:
        if(_nativeclosure(self)->_noutervalues > nval){
            _nativeclosure(self)->_outervalues[nval] = stack_get(v,-1);
        }
        else return sq_throwerror(v,_SC("invalid free var index"));
        break;
    default:
        return sq_aux_invalidtype(v, sq_type(self));
    }
    v->Pop();
    return SQ_OK;
}

/**
 * @brief クラスのメンバー属性を設定する。
 *
 * @details
 * スタックの `idx` にあるクラスオブジェクトのメンバー属性を設定する。メンバー名が null の場合は
 * クラス全体の属性を設定し、それ以外の場合は該当メンバーの属性を更新する。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効なインデックスの場合。
 */
SQRESULT sq_setattributes(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_CLASS,o);
    SQObjectPtr &key = stack_get(v,-2);
    SQObjectPtr &val = stack_get(v,-1);
    SQObjectPtr attrs;
    if(sq_type(key) == OT_NULL) {
        attrs = _class(*o)->_attributes;
        _class(*o)->_attributes = val;
        v->Pop(2);
        v->Push(attrs);
        return SQ_OK;
    }else if(_class(*o)->GetAttributes(key,attrs)) {
        _class(*o)->SetAttributes(key,val);
        v->Pop(2);
        v->Push(attrs);
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("wrong index"));
}

/**
 * @brief クラスのメンバー属性を取得する。
 *
 * @details
 * スタックの `idx` にあるクラスオブジェクトからメンバー属性を取得する。メンバー名が null の場合は
 * クラス全体の属性を取得し、それ以外の場合は該当メンバーの属性を取得してスタックにプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効なインデックスの場合。
 */
SQRESULT sq_getattributes(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_CLASS,o);
    SQObjectPtr &key = stack_get(v,-1);
    SQObjectPtr attrs;
    if(sq_type(key) == OT_NULL) {
        attrs = _class(*o)->_attributes;
        v->Pop();
        v->Push(attrs);
        return SQ_OK;
    }
    else if(_class(*o)->GetAttributes(key,attrs)) {
        v->Pop();
        v->Push(attrs);
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("wrong index"));
}

/**
 * @brief クラスメンバーのハンドルを取得する。
 *
 * @details
 * スタックの `idx` にあるクラスオブジェクトから、指定されたキーに対応する
 * メンバーのハンドル情報を取得し、`handle` に格納する。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @param[out] handle 取得したメンバーのハンドル情報。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効なインデックスの場合。
 */
SQRESULT sq_getmemberhandle(HSQUIRRELVM v,SQInteger idx,HSQMEMBERHANDLE *handle)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_CLASS,o);
    SQObjectPtr &key = stack_get(v,-1);
    SQTable *m = _class(*o)->_members;
    SQObjectPtr val;
    if(m->Get(key,val)) {
        handle->_static = _isfield(val) ? SQFalse : SQTrue;
        handle->_index = _member_idx(val);
        v->Pop();
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("wrong index"));
}

/**
 * @brief メンバーのハンドル情報から実体を取得する。
 *
 * @details
 * 指定されたクラスまたはインスタンスのメンバーを `handle` で指定し、
 * 実際の値ポインタを `val` に格納する。型がクラスまたはインスタンスでない場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param self 対象オブジェクト。
 * @param handle メンバーのハンドル情報。
 * @param[out] val メンバー値へのポインタ。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 無効な型の場合。
 */
SQRESULT _getmemberbyhandle(HSQUIRRELVM v,SQObjectPtr &self,const HSQMEMBERHANDLE *handle,SQObjectPtr *&val)
{
    switch(sq_type(self)) {
        case OT_INSTANCE: {
                SQInstance *i = _instance(self);
                if(handle->_static) {
                    SQClass *c = i->_class;
                    val = &c->_methods[handle->_index].val;
                }
                else {
                    val = &i->_values[handle->_index];

                }
            }
            break;
        case OT_CLASS: {
                SQClass *c = _class(self);
                if(handle->_static) {
                    val = &c->_methods[handle->_index].val;
                }
                else {
                    val = &c->_defaultvalues[handle->_index].val;
                }
            }
            break;
        default:
            return sq_throwerror(v,_SC("wrong type(expected class or instance)"));
    }
    return SQ_OK;
}

/**
 * @brief メンバーのハンドル情報から値を取得する。
 *
 * @details
 * スタックの `idx` にあるクラスまたはインスタンスオブジェクトから、
 * 指定されたメンバーのハンドルを使って値を取得し、スタックにプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param handle メンバーのハンドル情報。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 取得失敗。
 */
SQRESULT sq_getbyhandle(HSQUIRRELVM v,SQInteger idx,const HSQMEMBERHANDLE *handle)
{
    SQObjectPtr &self = stack_get(v,idx);
    SQObjectPtr *val = NULL;
    if(SQ_FAILED(_getmemberbyhandle(v,self,handle,val))) {
        return SQ_ERROR;
    }
    v->Push(_realval(*val));
    return SQ_OK;
}

/**
 * @brief メンバーのハンドル情報を使って値を設定する。
 *
 * @details
 * スタックの `idx` にあるクラスまたはインスタンスオブジェクトの、指定されたハンドルのメンバーに
 * スタックトップの値を設定する。
 *
 * @param v 対象の仮想マシン。
 * @param idx スタックインデックス。
 * @param handle メンバーのハンドル情報。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_setbyhandle(HSQUIRRELVM v,SQInteger idx,const HSQMEMBERHANDLE *handle)
{
    SQObjectPtr &self = stack_get(v,idx);
    SQObjectPtr &newval = stack_get(v,-1);
    SQObjectPtr *val = NULL;
    if(SQ_FAILED(_getmemberbyhandle(v,self,handle,val))) {
        return SQ_ERROR;
    }
    *val = newval;
    v->Pop();
    return SQ_OK;
}

/**
 * @brief クラスの基底クラスを取得する。
 *
 * @details
 * スタックの `idx` にあるクラスオブジェクトの基底クラスを取得し、
 * 存在すればスタックにプッシュする。基底クラスがない場合は null をプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getbase(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_CLASS,o);
    if(_class(*o)->_base)
        v->Push(SQObjectPtr(_class(*o)->_base));
    else
        v->PushNull();
    return SQ_OK;
}

/**
 * @brief インスタンスのクラスを取得する。
 *
 * @details
 * スタックの `idx` にあるインスタンスオブジェクトのクラス情報を取得し、
 * スタックにプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象インスタンスのスタックインデックス。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_getclass(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_INSTANCE,o);
    v->Push(SQObjectPtr(_instance(*o)->_class));
    return SQ_OK;
}

/**
 * @brief クラスから新しいインスタンスを生成する。
 *
 * @details
 * スタックの `idx` にあるクラスオブジェクトから新しいインスタンスを生成し、
 * スタックにプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象クラスのスタックインデックス。
 * @retval SQ_OK 成功。
 */
SQRESULT sq_createinstance(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_CLASS,o);
    v->Push(_class(*o)->CreateInstance());
    return SQ_OK;
}

/**
 * @brief 弱参照を生成する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトが参照カウント型の場合、その弱参照を生成して
 * スタックにプッシュする。参照カウント型でない場合はそのままプッシュする。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 */
void sq_weakref(HSQUIRRELVM v,SQInteger idx)
{
    SQObject &o=stack_get(v,idx);
    if(ISREFCOUNTED(sq_type(o))) {
        v->Push(_refcounted(o)->GetWeakRef(sq_type(o)));
        return;
    }
    v->Push(o);
}

/**
 * @brief 弱参照から値を取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトが弱参照型の場合、その参照先の値を取得して
 * スタックにプッシュする。弱参照型でない場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param idx 弱参照オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_getweakrefval(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &o = stack_get(v,idx);
    if(sq_type(o) != OT_WEAKREF) {
        return sq_throwerror(v,_SC("the object must be a weakref"));
    }
    v->Push(_weakref(o)->_obj);
    return SQ_OK;
}

/**
 * @brief デフォルトデリゲートを取得する。
 *
 * @details
 * 指定された型 `t` に対応するデフォルトデリゲートを取得し、スタックにプッシュする。
 * サポートされていない型の場合はエラーを返す。
 *
 * @param v 対象の仮想マシン。
 * @param t デリゲートを取得する対象の型。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 対応しない型の場合。
 */
SQRESULT sq_getdefaultdelegate(HSQUIRRELVM v,SQObjectType t)
{
    SQSharedState *ss = _ss(v);
    switch(t) {
    case OT_TABLE: v->Push(ss->_table_default_delegate); break;
    case OT_ARRAY: v->Push(ss->_array_default_delegate); break;
    case OT_STRING: v->Push(ss->_string_default_delegate); break;
    case OT_INTEGER: case OT_FLOAT: v->Push(ss->_number_default_delegate); break;
    case OT_GENERATOR: v->Push(ss->_generator_default_delegate); break;
    case OT_CLOSURE: case OT_NATIVECLOSURE: v->Push(ss->_closure_default_delegate); break;
    case OT_THREAD: v->Push(ss->_thread_default_delegate); break;
    case OT_CLASS: v->Push(ss->_class_default_delegate); break;
    case OT_INSTANCE: v->Push(ss->_instance_default_delegate); break;
    case OT_WEAKREF: v->Push(ss->_weakref_default_delegate); break;
    default: return sq_throwerror(v,_SC("the type doesn't have a default delegate"));
    }
    return SQ_OK;
}

/**
 * @brief イテレータを進め、次の要素を取得する。
 *
 * @details
 * スタックの `idx` にあるオブジェクトを対象に、スタックトップの参照位置から
 * 次の要素を取得する。取得に成功すると、キーと値をスタックにプッシュする。
 * ジェネレータには使用できない。
 *
 * @param v 対象の仮想マシン。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_next(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr o=stack_get(v,idx),&refpos = stack_get(v,-1),realkey,val;
    if(sq_type(o) == OT_GENERATOR) {
        return sq_throwerror(v,_SC("cannot iterate a generator"));
    }
    int faketojump;
    if(!v->FOREACH_OP(o,realkey,val,refpos,0,666,faketojump))
        return SQ_ERROR;
    if(faketojump != 666) {
        v->Push(realkey);
        v->Push(val);
        return SQ_OK;
    }
    return SQ_ERROR;
}

/**
 * @struct BufState
 * @brief バッファの状態を保持する構造体。
 *
 * @details
 * スクリプトバッファを解析する際に使用する読み込み位置などの情報を保持する。
 * `buf` は読み込む文字列、`ptr` は現在位置、`size` はバッファサイズを示す。
 */
struct BufState{
    const SQChar *buf;
    SQInteger ptr;
    SQInteger size;
};

/**
 * @brief バッファから1文字読み込む。
 *
 * @details
 * `BufState` 構造体で管理されているバッファから現在位置の文字を読み込み、
 * ポインタを進める。バッファ終端に到達した場合は 0 を返す。
 *
 * @param file 読み込み対象の BufState へのポインタ。
 * @return SQInteger 読み込んだ文字、終端の場合は 0。
 */
SQInteger buf_lexfeed(SQUserPointer file)
{
    BufState *buf=(BufState*)file;
    if(buf->size<(buf->ptr+1))
        return 0;
    return buf->buf[buf->ptr++];
}

/**
 * @brief バッファからスクリプトをコンパイルする。
 *
 * @details
 * 与えられた文字列バッファをソースとしてスクリプトをコンパイルする。
 * 内部で `buf_lexfeed` を使用して文字を読み込み、スクリプトを解析する。
 *
 * @param v 対象の仮想マシン。
 * @param s コンパイル対象の文字列。
 * @param size バッファサイズ。
 * @param sourcename ソース名（エラー出力用など）。
 * @param raiseerror コンパイルエラー時に例外を送出するかどうか。
 * @retval SQ_OK 成功。
 * @retval SQ_ERROR 失敗。
 */
SQRESULT sq_compilebuffer(HSQUIRRELVM v,const SQChar *s,SQInteger size,const SQChar *sourcename,SQBool raiseerror) {
    BufState buf;
    buf.buf = s;
    buf.size = size;
    buf.ptr = 0;
    return sq_compile(v, buf_lexfeed, &buf, sourcename, raiseerror);
}

/**
 * @brief スタックの値を他の VM に移動する。
 *
 * @details
 * `src` VM のスタックの `idx` にある値を `dest` VM にプッシュする。
 * VM 間でオブジェクトを共有したい場合に使用する。
 *
 * @param dest コピー先の仮想マシン。
 * @param src コピー元の仮想マシン。
 * @param idx コピー元スタックインデックス。
 */
void sq_move(HSQUIRRELVM dest,HSQUIRRELVM src,SQInteger idx)
{
    dest->Push(stack_get(src,idx));
}

/**
 * @brief 出力関数とエラー出力関数を設定する。
 *
 * @details
 * VM が printf などで使用する出力関数とエラー出力関数を設定する。
 * スクリプトのログ出力やエラーログをカスタマイズする際に使用する。
 *
 * @param v 対象の仮想マシン。
 * @param printfunc 標準出力用のコールバック関数。
 * @param errfunc エラー出力用のコールバック関数。
 */
void sq_setprintfunc(HSQUIRRELVM v, SQPRINTFUNCTION printfunc,SQPRINTFUNCTION errfunc)
{
    _ss(v)->_printfunc = printfunc;
    _ss(v)->_errorfunc = errfunc;
}

/**
 * @brief 出力関数を取得する。
 *
 * @details
 * 設定されている標準出力用の printf 関数を取得する。
 *
 * @param v 対象の仮想マシン。
 * @return SQPRINTFUNCTION 設定されている出力関数。
 */
SQPRINTFUNCTION sq_getprintfunc(HSQUIRRELVM v)
{
    return _ss(v)->_printfunc;
}

/**
 * @brief エラー出力関数を取得する。
 *
 * @details
 * 設定されているエラー出力用の printf 関数を取得する。
 *
 * @param v 対象の仮想マシン。
 * @return SQPRINTFUNCTION 設定されているエラー出力関数。
 */
SQPRINTFUNCTION sq_geterrorfunc(HSQUIRRELVM v)
{
    return _ss(v)->_errorfunc;
}

/**
 * @brief メモリを確保する。
 *
 * @details
 * 指定されたサイズのメモリを確保して返す。
 * スクリプト実行中の内部使用やカスタムアロケータに利用される。
 *
 * @param size 確保するメモリサイズ（バイト）。
 * @return void* 確保されたメモリ領域へのポインタ。
 */
void *sq_malloc(SQUnsignedInteger size)
{
    return SQ_MALLOC(size);
}

/**
 * @brief メモリを再確保する。
 *
 * @details
 * 既存のメモリ領域 `p` を新しいサイズ `newsize` に再確保する。
 * 元のサイズ `oldsize` を指定することで適切に再割り当てされる。
 *
 * @param p 再確保するメモリブロック。
 * @param oldsize 元のサイズ。
 * @param newsize 新しいサイズ。
 * @return void* 再確保されたメモリ領域へのポインタ。
 */
void *sq_realloc(void* p,SQUnsignedInteger oldsize,SQUnsignedInteger newsize)
{
    return SQ_REALLOC(p,oldsize,newsize);
}

/**
 * @brief メモリを解放する。
 *
 * @details
 * 指定されたメモリ領域 `p` を解放する。`size` は解放するメモリのサイズで、
 * 内部アロケータに渡される。
 *
 * @param p 解放するメモリブロックへのポインタ。
 * @param size 解放するメモリのサイズ（バイト）。
 */
void sq_free(void *p,SQUnsignedInteger size)
{
    SQ_FREE(p,size);
}
