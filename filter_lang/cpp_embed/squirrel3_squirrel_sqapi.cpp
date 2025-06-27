//cpp

/* Written By Gemini Pro*/

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
 * @brief 指定されたスタックインデックスの引数が期待される型であるかを確認し、そのオブジェクトポインタを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx スタックインデックス。
 * @param type 期待されるオブジェクトの型。
 * @param o オブジェクトポインタを格納するポインタ。
 * @return 型が一致すればtrue、そうでなければエラーを発生させてfalseを返します。
 * @details この関数は内部ヘルパーであり、API関数の引数型チェックを簡素化するために使用されます。
 * 型が一致しない場合、VMに型エラーを報告します。
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

// マクロ: 型安全なオブジェクト取得
#define _GETSAFE_OBJ(v,idx,type,o) { if(!sq_aux_gettypedarg(v,idx,type,&o)) return SQ_ERROR; }

// マクロ: パラメータ数のチェック
#define sq_aux_paramscheck(v,count) \
{ \
    if(sq_gettop(v) < count){ v->Raise_Error(_SC("not enough params in the stack")); return SQ_ERROR; }\
}

/**
 * @brief 予期しない型が指定された場合にエラーをスローします。
 * @param v 対象のSquirrel VM。
 * @param type 予期しなかったオブジェクトの型。
 * @return エラーコード (SQ_ERROR)。
 * @details この関数は、特定の操作でサポートされていない型のオブジェクトが渡されたときに、
 * 適切なエラーメッセージを生成してVMに報告するために使用されます。
 */
SQInteger sq_aux_invalidtype(HSQUIRRELVM v,SQObjectType type)
{
    SQUnsignedInteger buf_size = 100 *sizeof(SQChar);
    scsprintf(_ss(v)->GetScratchPad(buf_size), buf_size, _SC("unexpected type %s"), IdType2Name(type));
    return sq_throwerror(v, _ss(v)->GetScratchPad(-1));
}

/**
 * @brief 新しいSquirrel VMインスタンスを作成し、開きます。
 * @param initialstacksize VMの初期スタックサイズ。
 * @return 成功した場合は新しいVMへのハンドル。失敗した場合はNULL。
 * @details この関数は、Squirrelスクリプトを実行するための主要なコンテナである新しいVMを作成します。
 * すべてのVMは、ガベージコレクタや定数テーブルなどのグローバル情報を格納する
 * 共有状態(SQSharedState)を共有します。
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
 * @brief 既存のVMと共有状態を共有する新しいスレッド（軽量VM）を作成します。
 * @param friendvm 新しいスレッドが共有状態を共有する既存のVM（フレンドVM）。
 * @param initialstacksize 新しいスレッドの初期スタックサイズ。
 * @return 成功した場合は新しいスレッドVMへのハンドル。失敗した場合はNULL。
 * @details スレッドはコルーチンの実装に使用されます。フレンドVMのスタックに新しいスレッドオブジェクトをプッシュします。
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
 * @brief VMの現在の状態を取得します。
 * @param v 対象のSquirrel VM。
 * @return VMの状態を示す値 (SQ_VMSTATE_IDLE, SQ_VMSTATE_RUNNING, SQ_VMSTATE_SUSPENDED)。
 * @details VMの状態は以下の通りです:
 * - SQ_VMSTATE_IDLE: VMは現在何も実行していません。
 * - SQ_VMSTATE_RUNNING: VMは現在スクリプトを実行中です。
 * - SQ_VMSTATE_SUSPENDED: VMは中断されています。
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
 * @brief VMのグローバルエラーハンドラを設定します。
 * @param v 対象のSquirrel VM。
 * @details スタックのトップにあるクロージャまたはネイティブクロージャをエラーハンドラとして設定します。
 * エラーが発生すると、このハンドラが呼び出されます。NULLを設定すると、エラーハンドラは削除されます。
 * 設定後、スタックトップのオブジェクトはポップされます。
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
 * @brief ネイティブコード（C/C++）で実装されたデバッグフックを設定します。
 * @param v 対象のSquirrel VM。
 * @param hook 設定するデバッグフック関数へのポインタ。
 * @details デバッグフックは、VMの実行中に特定のイベント（行の変更、関数の呼び出し/リターンなど）が発生したときに呼び出されます。
 * `hook`にNULLを設定すると、ネイティブデバッグフックは無効になります。
 */
void sq_setnativedebughook(HSQUIRRELVM v,SQDEBUGHOOK hook)
{
    v->_debughook_native = hook;
    v->_debughook_closure.Null();
    v->_debughook = hook?true:false;
}

/**
 * @brief Squirrelスクリプトで実装されたデバッグフックを設定します。
 * @param v 対象のSquirrel VM。
 * @details スタックのトップにあるクロージャをデバッグフックとして設定します。
 * `sq_setnativedebughook`と同様に、VMの実行中に特定のイベントで呼び出されます。
 * スタックトップのオブジェクトがNULLの場合、デバッグフックは無効になります。
 * 設定後、スタックトップのオブジェクトはポップされます。
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
 * @brief VMを閉じ、関連するすべてのリソースを解放します。
 * @param v 閉じる対象のSquirrel VM。
 * @details この関数は、ルートVMに対してのみ呼び出すべきです。
 * 関連するすべてのスレッドと共有状態が破棄されます。
 */
void sq_close(HSQUIRRELVM v)
{
    SQSharedState *ss = _ss(v);
    _thread(ss->_root_vm)->Finalize();
    sq_delete(ss, SQSharedState);
}

/**
 * @brief Squirrelのバージョン番号を取得します。
 * @return Squirrelのバージョンを表す整数。 (例: 301はバージョン3.0.1を意味します)
 */
SQInteger sq_getversion()
{
    return SQUIRREL_VERSION_NUMBER;
}

/**
 * @brief Squirrelソースコードを読み込み、コンパイルして実行可能なクロージャを生成します。
 * @param v 対象のSquirrel VM。
 * @param read ソースコードを読み込むためのコールバック関数。
 * @param p `read`関数に渡されるユーザーポインタ。
 * @param sourcename ソースコードの名称（デバッグ情報に使用）。
 * @param raiseerror コンパイルエラーが発生した場合にVMにエラーを発生させるかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details コンパイルが成功すると、結果として得られるルートクロージャがスタックにプッシュされます。
 * NO_COMPILERが定義されているビルドでは、この関数は常にエラーを返します。
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
 * @brief デバッグ情報の生成を有効または無効にします。
 * @param v 対象のSquirrel VM。
 * @param enable trueで有効、falseで無効。
 * @details デバッグ情報を有効にすると、コンパイラは行番号やローカル変数名などの情報をバイトコードに埋め込みます。
 * これにより、デバッグが容易になりますが、バイトコードのサイズは増加します。
 * この設定は共有状態に影響し、すべての関連VMに適用されます。
 */
void sq_enabledebuginfo(HSQUIRRELVM v, SQBool enable)
{
    _ss(v)->_debuginfo = enable?true:false;
}

/**
 * @brief すべての例外を通知するかどうかを設定します。
 * @param v 対象のSquirrel VM。
 * @param enable trueで有効、falseで無効。
 * @details この機能が有効な場合、キャッチされた例外でもデバッグフックがトリガーされます。
 * デフォルトでは、キャッチされなかった例外のみが通知されます。
 * この設定は共有状態に影響します。
 */
void sq_notifyallexceptions(HSQUIRRELVM v, SQBool enable)
{
    _ss(v)->_notifyallexceptions = enable?true:false;
}

/**
 * @brief Squirrelオブジェクトの参照カウントをインクリメントします。
 * @param v 対象のSquirrel VM。
 * @param po 参照カウントを増やすオブジェクトへのポインタ。
 * @details 参照カウントを持つオブジェクト（テーブル、配列、文字列など）に対してのみ有効です。
 * これにより、オブジェクトがガベージコレクタによって解放されるのを防ぎます。
 * `sq_release`と対で使われる必要があります。
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
 * @brief Squirrelオブジェクトの現在の参照カウントを取得します。
 * @param v 対象のSquirrel VM。
 * @param po 対象のオブジェクトへのポインタ。
 * @return オブジェクトの参照カウント。参照カウントされない型の場合は0。
 * @details この関数は主にデバッグ目的で使用されます。
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
 * @brief Squirrelオブジェクトの参照カウントをデクリメントします。
 * @param v 対象のSquirrel VM。
 * @param po 参照カウントを減らすオブジェクトへのポインタ。
 * @return 参照カウントが0になった場合にtrue、それ以外はfalse。
 * @details 参照カウントが0になると、オブジェクトは解放される可能性があります。
 * `sq_addref`で取得した参照は、必ずこの関数で解放する必要があります。
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
 * @brief Squirrel VM内部のオブジェクトの参照カウントを取得します。
 * @param v 対象のSquirrel VM (現在は未使用)。
 * @param po 対象のオブジェクトへのポインタ。
 * @return オブジェクトの内部参照カウント。参照カウントされない型の場合は0。
 * @details `sq_getrefcount`とは異なり、この関数はガベージコレクタの参照テーブルではなく、
 * オブジェクト自体の参照カウンタを直接返します。
 */
SQUnsignedInteger sq_getvmrefcount(HSQUIRRELVM SQ_UNUSED_ARG(v), const HSQOBJECT *po)
{
    if (!ISREFCOUNTED(sq_type(*po))) return 0;
    return po->_unVal.pRefCounted->_uiRef;
}

/**
 * @brief オブジェクトが文字列型の場合、そのC文字列ポインタを取得します。
 * @param o 対象のオブジェクトへのポインタ。
 * @return オブジェクトが文字列であればその内容のC文字列ポインタ、そうでなければNULL。
 */
const SQChar *sq_objtostring(const HSQOBJECT *o)
{
    if(sq_type(*o) == OT_STRING) {
        return _stringval(*o);
    }
    return NULL;
}

/**
 * @brief オブジェクトが数値型の場合、その整数値を取得します。
 * @param o 対象のオブジェクトへのポインタ。
 * @return オブジェクトが数値であればその整数値、そうでなければ0。
 */
SQInteger sq_objtointeger(const HSQOBJECT *o)
{
    if(sq_isnumeric(*o)) {
        return tointeger(*o);
    }
    return 0;
}

/**
 * @brief オブジェクトが数値型の場合、その浮動小数点数値を取得します。
 * @param o 対象のオブジェクトへのポインタ。
 * @return オブジェクトが数値であればその浮動小数点数値、そうでなければ0.0。
 */
SQFloat sq_objtofloat(const HSQOBJECT *o)
{
    if(sq_isnumeric(*o)) {
        return tofloat(*o);
    }
    return 0;
}

/**
 * @brief オブジェクトがブール型の場合、そのブール値を取得します。
 * @param o 対象のオブジェクトへのポインタ。
 * @return オブジェクトがブール値であればその値 (SQTrue/SQFalse)、そうでなければSQFalse。
 */
SQBool sq_objtobool(const HSQOBJECT *o)
{
    if(sq_isbool(*o)) {
        return _integer(*o);
    }
    return SQFalse;
}

/**
 * @brief オブジェクトがユーザーポインタ型の場合、その値を取得します。
 * @param o 対象のオブジェクトへのポインタ。
 * @return オブジェクトがユーザーポインタであればそのポインタ値、そうでなければNULL。
 */
SQUserPointer sq_objtouserpointer(const HSQOBJECT *o)
{
    if(sq_isuserpointer(*o)) {
        return _userpointer(*o);
    }
    return 0;
}

/**
 * @brief スタックにnull値をプッシュします。
 * @param v 対象のSquirrel VM。
 */
void sq_pushnull(HSQUIRRELVM v)
{
    v->PushNull();
}

/**
 * @brief スタックに文字列をプッシュします。
 * @param v 対象のSquirrel VM。
 * @param s プッシュするC文字列。
 * @param len 文字列の長さ。-1の場合、文字列はnull終端されていると見なされます。
 */
void sq_pushstring(HSQUIRRELVM v,const SQChar *s,SQInteger len)
{
    if(s)
        v->Push(SQObjectPtr(SQString::Create(_ss(v), s, len)));
    else v->PushNull();
}

/**
 * @brief スタックに整数値をプッシュします。
 * @param v 対象のSquirrel VM。
 * @param n プッシュする整数。
 */
void sq_pushinteger(HSQUIRRELVM v,SQInteger n)
{
    v->Push(n);
}

/**
 * @brief スタックにブール値をプッシュします。
 * @param v 対象のSquirrel VM。
 * @param b プッシュするブール値 (trueまたはfalse)。
 */
void sq_pushbool(HSQUIRRELVM v,SQBool b)
{
    v->Push(b?true:false);
}

/**
 * @brief スタックに浮動小数点数値をプッシュします。
 * @param v 対象のSquirrel VM。
 * @param n プッシュする浮動小数点数。
 */
void sq_pushfloat(HSQUIRRELVM v,SQFloat n)
{
    v->Push(n);
}

/**
 * @brief スタックにユーザーポインタをプッシュします。
 * @param v 対象のSquirrel VM。
 * @param p プッシュするユーザーポインタ。
 * @details ユーザーポインタは、SquirrelからC/C++の任意のデータを参照するための軽量な方法です。
 */
void sq_pushuserpointer(HSQUIRRELVM v,SQUserPointer p)
{
    v->Push(p);
}

/**
 * @brief スタックにスレッドオブジェクトをプッシュします。
 * @param v 対象のSquirrel VM。
 * @param thread プッシュするスレッドVMのハンドル。
 */
void sq_pushthread(HSQUIRRELVM v, HSQUIRRELVM thread)
{
    v->Push(thread);
}

/**
 * @brief 新しいユーザーデータを作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param size 確保するメモリ領域のサイズ（バイト単位）。
 * @return 確保されたメモリ領域へのポインタ。
 * @details ユーザーデータは、Squirrel VMによって管理されるメモリブロックです。
 * ガベージコレクトされ、デリゲートを設定してメタメソッドを持つことができます。
 */
SQUserPointer sq_newuserdata(HSQUIRRELVM v,SQUnsignedInteger size)
{
    SQUserData *ud = SQUserData::Create(_ss(v), size + SQ_ALIGNMENT);
    v->Push(ud);
    return (SQUserPointer)sq_aligning(ud + 1);
}

/**
 * @brief 新しい空のテーブルを作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 */
void sq_newtable(HSQUIRRELVM v)
{
    v->Push(SQTable::Create(_ss(v), 0));
}

/**
 * @brief 指定された初期容量で新しいテーブルを作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param initialcapacity テーブルの初期容量（要素数）。
 */
void sq_newtableex(HSQUIRRELVM v,SQInteger initialcapacity)
{
    v->Push(SQTable::Create(_ss(v), initialcapacity));
}

/**
 * @brief 指定されたサイズの新しい配列を作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param size 配列の初期サイズ。
 */
void sq_newarray(HSQUIRRELVM v,SQInteger size)
{
    v->Push(SQArray::Create(_ss(v), size));
}

/**
 * @brief 新しいクラスを作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param hasbase trueの場合、スタックトップのクラスを基底クラスとして継承します。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details `hasbase`がtrueの場合、この関数を呼び出す前に基底クラスをスタックにプッシュしておく必要があります。
 * 成功すると、基底クラスはスタックからポップされ、新しいクラスがプッシュされます。
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
 * @brief インスタンスが特定のクラスのインスタンス（またはその派生クラスのインスタンス）であるかを確認します。
 * @param v 対象のSquirrel VM。
 * @return インスタンスがクラスに属する場合はSQTrue、そうでない場合はSQFalse。エラーの場合はSQ_ERROR。
 * @details この関数を呼び出す前に、スタックにクラス、その次にインスタンスの順でプッシュしておく必要があります。
 * (スタック: ..., class, instance)
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
 * @brief 配列の末尾に要素を追加します。
 * @param v 対象のSquirrel VM。
 * @param idx 配列が格納されているスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、追加する要素をスタックトップにプッシュしておく必要があります。
 * 追加後、その要素はスタックからポップされます。
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
 * @brief 配列の末尾の要素を削除（ポップ）します。
 * @param v 対象のSquirrel VM。
 * @param idx 配列が格納されているスタックインデックス。
 * @param pushval trueの場合、削除された要素をスタックにプッシュします。
 * @return 成功した場合はSQ_OK、配列が空の場合はSQ_ERROR。
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
 * @brief 配列のサイズを変更します。
 * @param v 対象のSquirrel VM。
 * @param idx 配列が格納されているスタックインデックス。
 * @param newsize 新しい配列のサイズ。
 * @return 成功した場合はSQ_OK、`newsize`が負の場合はSQ_ERROR。
 * @details サイズを大きくした場合、新しい要素はnullで初期化されます。
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
 * @brief 配列の要素の順序を反転させます。
 * @param v 対象のSquirrel VM。
 * @param idx 配列が格納されているスタックインデックス。
 * @return 常にSQ_OK。
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
 * @brief 配列から指定したインデックスの要素を削除します。
 * @param v 対象のSquirrel VM。
 * @param idx 配列が格納されているスタックインデックス。
 * @param itemidx 削除する要素のインデックス。
 * @return 成功した場合はSQ_OK、インデックスが範囲外の場合はSQ_ERROR。
 */
SQRESULT sq_arrayremove(HSQUIRRELVM v,SQInteger idx,SQInteger itemidx)
{
    sq_aux_paramscheck(v, 1);
    SQObjectPtr *arr;
    _GETSAFE_OBJ(v, idx, OT_ARRAY,arr);
    return _array(*arr)->Remove(itemidx) ? SQ_OK : sq_throwerror(v,_SC("index out of range"));
}

/**
 * @brief 配列の指定した位置に要素を挿入します。
 * @param v 対象のSquirrel VM。
 * @param idx 配列が格納されているスタックインデックス。
 * @param destpos 挿入先のインデックス。
 * @return 成功した場合はSQ_OK、インデックスが範囲外の場合はSQ_ERROR。
 * @details この関数を呼び出す前に、挿入する要素をスタックトップにプッシュしておく必要があります。
 * 挿入後、その要素はスタックからポップされます。
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
 * @brief C/C++関数から新しいネイティブクロージャを作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param func Squirrelから呼び出されるC/C++関数へのポインタ。
 * @param nfreevars クロージャがキャプチャする自由変数の数。
 * @details この関数を呼び出す前に、`nfreevars`個の自由変数をスタックにプッシュしておく必要があります。
 * これらの自由変数はクロージャにバインドされ、スタックからポップされます。
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
 * @brief クロージャに関する情報（パラメータ数、自由変数の数）を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャが格納されているスタックインデックス。
 * @param nparams パラメータの数を格納するポインタ。
 * @param nfreevars 自由変数の数を格納するポインタ。
 * @return 成功した場合はSQ_OK、対象がクロージャでない場合はSQ_ERROR。
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
 * @brief ネイティブクロージャに名前を設定します。
 * @param v 対象のSquirrel VM。
 * @param idx ネイティブクロージャが格納されているスタックインデックス。
 * @param name 設定する名前。
 * @return 成功した場合はSQ_OK、対象がネイティブクロージャでない場合はSQ_ERROR。
 * @details この名前はデバッグ情報やエラーメッセージに使用されます。
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
 * @brief ネイティブクロージャのパラメータ数チェックと型チェックを設定します。
 * @param v 対象のSquirrel VM。
 * @param nparamscheck 期待されるパラメータの数。可変長引数の場合は `SQ_MATCHTYPEMASKSTRING` を使用します。
 * @param typemask パラメータの型を定義するマスク文字列 (例: ".is n")。NULLの場合は型チェックを行いません。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数は、スタックトップのネイティブクロージャに対して設定を行います。
 * `nparamscheck` に正の値を設定すると、呼び出し時の引数と数とを比較します。
 * 負の値を設定すると、少なくとも `abs(nparamscheck)` 個の引数が必要であることを意味します。
 * `typemask` を使用すると、各パラメータの型を自動的に検証できます。
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
 * @brief クロージャに環境オブジェクト（`this`として参照されるオブジェクト）をバインドします。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャが格納されているスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、環境として設定するオブジェクト（テーブル、クラス、インスタンス等）を
 * スタックトップにプッシュしておく必要があります。
 * この関数は元のクロージャを変更せず、環境がバインドされた新しいクロージャを生成してスタックにプッシュします。
 * 環境オブジェクトはスタックからポップされます。
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
 * @brief クロージャの名前を取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャが格納されているスタックインデックス。
 * @return 成功した場合はSQ_OK、対象がクロージャでない場合はSQ_ERROR。
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
 * @brief スクリプトクロージャのルートテーブルを設定します。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャが格納されているスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、新しいルートテーブルをスタックトップにプッシュしておく必要があります。
 * ルートテーブルは自由変数が見つからなかった場合のフォールバックとして使用されます。
 * 設定後、テーブルはスタックからポップされます。
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
 * @brief スクリプトクロージャの現在のルートテーブルを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャが格納されているスタックインデックス。
 * @return 成功した場合はSQ_OK、対象がクロージャでない場合はSQ_ERROR。
 */
SQRESULT sq_getclosureroot(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr &c = stack_get(v,idx);
    if(!sq_isclosure(c)) return sq_throwerror(v, _SC("closure expected"));
    v->Push(_closure(c)->_root->_obj);
    return SQ_OK;
}

/**
 * @brief テーブルまたは配列のすべての要素を削除します。
 * @param v 対象のSquirrel VM。
 * @param idx テーブルまたは配列が格納されているスタックインデックス。
 * @return 成功した場合はSQ_OK、対象がテーブルまたは配列でない場合はSQ_ERROR。
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
 * @brief VMの現在のルートテーブルをスタックにプッシュします。
 * @param v 対象のSquirrel VM。
 */
void sq_pushroottable(HSQUIRRELVM v)
{
    v->Push(v->_roottable);
}

/**
 * @brief レジストリテーブルをスタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @details レジストリテーブルは、VMの生存期間中にCコードがSquirrelの値を格納するために使用できるグローバルなテーブルです。
 */
void sq_pushregistrytable(HSQUIRRELVM v)
{
    v->Push(_ss(v)->_registry);
}

/**
 * @brief 定数テーブルをスタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @details 定数テーブルには、`const` キーワードで宣言された定数が格納されます。
 */
void sq_pushconsttable(HSQUIRRELVM v)
{
    v->Push(_ss(v)->_consts);
}

/**
 * @brief VMの現在のルートテーブルを設定します。
 * @param v 対象のSquirrel VM。
 * @return 成功した場合はSQ_OK、スタックトップがテーブルまたはnullでない場合はSQ_ERROR。
 * @details この関数を呼び出す前に、新しいルートテーブルをスタックトップにプッシュしておく必要があります。
 * 設定後、テーブルはスタックからポップされます。
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
 * @brief VMの定数テーブルを設定します。
 * @param v 対象のSquirrel VM。
 * @return 成功した場合はSQ_OK、スタックトップがテーブルでない場合はSQ_ERROR。
 * @details この関数を呼び出す前に、新しい定数テーブルをスタックトップにプッシュしておく必要があります。
 * 設定後、テーブルはスタックからポップされます。この設定は共有状態に影響します。
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
 * @brief VMに外部のポインタ（foreign pointer）を設定します。
 * @param v 対象のSquirrel VM。
 * @param p 設定するユーザーポインタ。
 * @details このポインタはVMに紐付けられ、ホストアプリケーションが任意のデータをVMインスタンスに関連付けるために使用できます。
 * Squirrel自体はこのポインタを使用しません。
 */
void sq_setforeignptr(HSQUIRRELVM v,SQUserPointer p)
{
    v->_foreignptr = p;
}

/**
 * @brief VMに設定されている外部ポインタを取得します。
 * @param v 対象のSquirrel VM。
 * @return 以前に `sq_setforeignptr` で設定されたポインタ。
 */
SQUserPointer sq_getforeignptr(HSQUIRRELVM v)
{
    return v->_foreignptr;
}

/**
 * @brief VMの共有状態に外部のポインタを設定します。
 * @param v 対象のSquirrel VM。
 * @param p 設定するユーザーポインタ。
 * @details このポインタは共有状態に紐付けられ、関連するすべてのVMからアクセス可能です。
 */
void sq_setsharedforeignptr(HSQUIRRELVM v,SQUserPointer p)
{
    _ss(v)->_foreignptr = p;
}

/**
 * @brief VMの共有状態に設定されている外部ポインタを取得します。
 * @param v 対象のSquirrel VM。
 * @return 以前に `sq_setsharedforeignptr` で設定されたポインタ。
 */
SQUserPointer sq_getsharedforeignptr(HSQUIRRELVM v)
{
    return _ss(v)->_foreignptr;
}

/**
 * @brief VM固有のリリースフックを設定します。
 * @param v 対象のSquirrel VM。
 * @param hook VMが破棄されるときに呼び出されるフック関数。
 * @details このフックは、`sq_close`が呼び出され、このVMが破棄される直前に呼び出されます。
 * 主にVMに関連付けられたリソースをクリーンアップするために使用されます。
 */
void sq_setvmreleasehook(HSQUIRRELVM v,SQRELEASEHOOK hook)
{
    v->_releasehook = hook;
}

/**
 * @brief VM固有のリリースフックを取得します。
 * @param v 対象のSquirrel VM。
 * @return 設定されているリリースフック関数へのポインタ。
 */
SQRELEASEHOOK sq_getvmreleasehook(HSQUIRRELVM v)
{
    return v->_releasehook;
}

/**
 * @brief 共有状態のリリースフックを設定します。
 * @param v 対象のSquirrel VM。
 * @param hook 共有状態が破棄されるときに呼び出されるフック関数。
 * @details このフックは、共有状態が破棄される（通常はルートVMが閉じられる）直前に呼び出されます。
 */
void sq_setsharedreleasehook(HSQUIRRELVM v,SQRELEASEHOOK hook)
{
    _ss(v)->_releasehook = hook;
}

/**
 * @brief 共有状態のリリースフックを取得します。
 * @param v 対象のSquirrel VM。
 * @return 設定されている共有リリースフック関数へのポインタ。
 */
SQRELEASEHOOK sq_getsharedreleasehook(HSQUIRRELVM v)
{
    return _ss(v)->_releasehook;
}


/**
 * @brief 指定されたインデックスのスタック上のオブジェクトをスタックトップにコピー（プッシュ）します。
 * @param v 対象のSquirrel VM。
 * @param idx コピー元のスタックインデックス。
 */
void sq_push(HSQUIRRELVM v,SQInteger idx)
{
    v->Push(stack_get(v, idx));
}

/**
 * @brief 指定されたインデックスのスタック上のオブジェクトの型を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return オブジェクトの型 (SQObjectType)。
 */
SQObjectType sq_gettype(HSQUIRRELVM v,SQInteger idx)
{
    return sq_type(stack_get(v, idx));
}

/**
 * @brief 指定されたインデックスのオブジェクトの型名を取得し、文字列としてスタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details これはSquirrelの`typeof`演算子に相当するAPIです。
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
 * @brief 指定されたインデックスのオブジェクトを文字列に変換し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details これはSquirrelの`tostring`関数に相当するAPIです。オブジェクトに `_tostring` メタメソッドが
 * 定義されていれば、それが呼び出されます。
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
 * @brief 指定されたインデックスのオブジェクトのブール値としての真偽を判定します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param b 結果のブール値を格納するポインタ。
 * @details Squirrelのルールに従い、`null`と`false`のみが偽と見なされ、それ以外はすべて真と見なされます。
 */
void sq_tobool(HSQUIRRELVM v, SQInteger idx, SQBool *b)
{
    SQObjectPtr &o = stack_get(v, idx);
    *b = SQVM::IsFalse(o)?SQFalse:SQTrue;
}

/**
 * @brief 指定されたインデックスのオブジェクトを整数に変換して取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param i 結果の整数を格納するポインタ。
 * @return 変換に成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details 対象オブジェクトが数値またはブール値である必要があります。
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
 * @brief 指定されたインデックスのオブジェクトを浮動小数点数に変換して取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param f 結果の浮動小数点数を格納するポインタ。
 * @return 変換に成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details 対象オブジェクトが数値である必要があります。
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
 * @brief 指定されたインデックスのオブジェクトがブール値であるか確認し、その値を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param b 結果のブール値を格納するポインタ。
 * @return 対象がブール値であればSQ_OK、そうでなければSQ_ERROR。
 */
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
 * @brief 指定されたインデックスの文字列オブジェクトから、C文字列ポインタと長さを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 文字列オブジェクトのスタックインデックス。
 * @param c 文字列へのポインタを格納するポインタ。
 * @param size 文字列の長さを格納するポインタ。
 * @return 対象が文字列であればSQ_OK、そうでなければSQ_ERROR。
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
 * @brief 指定されたインデックスの文字列オブジェクトから、C文字列ポインタを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 文字列オブジェクトのスタックインデックス。
 * @param c 文字列へのポインタを格納するポインタ。
 * @return 対象が文字列であればSQ_OK、そうでなければSQ_ERROR。
 */
SQRESULT sq_getstring(HSQUIRRELVM v,SQInteger idx,const SQChar **c)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_STRING,o);
    *c = _stringval(*o);
    return SQ_OK;
}

/**
 * @brief 指定されたインデックスのスレッドオブジェクトから、VMハンドルを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx スレッドオブジェクトのスタックインデックス。
 * @param thread VMハンドルを格納するポインタ。
 * @return 対象がスレッドであればSQ_OK、そうでなければSQ_ERROR。
 */
SQRESULT sq_getthread(HSQUIRRELVM v,SQInteger idx,HSQUIRRELVM *thread)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_THREAD,o);
    *thread = _thread(*o);
    return SQ_OK;
}

/**
 * @brief 指定されたインデックスのオブジェクトのクローン（ディープコピー）を作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx クローン元のオブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details テーブル、配列、クラスインスタンスなどのコンテナオブジェクトがクローン可能です。
 * クローンされたオブジェクトは、元のオブジェクトとは独立したコピーになります。
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
 * @brief 指定されたインデックスのオブジェクトのサイズを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return オブジェクトのサイズ。
 * @details サイズの意味はオブジェクトの型によって異なります:
 * - 文字列: 文字列の長さ
 * - テーブル: 格納されている要素の数
 * - 配列: 配列のサイズ
 * - ユーザーデータ: 確保されたメモリのサイズ
 * - クラス/インスタンス: ユーザーデータ領域のサイズ
 * サポートされていない型の場合はエラーがスローされます。
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
 * @brief 指定されたインデックスのオブジェクトのハッシュ値を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return オブジェクトのハッシュ値。
 * @details このハッシュ値は、オブジェクトがテーブルのキーとして使用される際に内部的に計算されるものと同じです。
 */
SQHash sq_gethash(HSQUIRRELVM v, SQInteger idx)
{
    SQObjectPtr &o = stack_get(v, idx);
    return HashObj(o);
}

/**
 * @brief 指定されたインデックスのユーザーデータオブジェクトから、データポインタと型タグを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx ユーザーデータオブジェクトのスタックインデックス。
 * @param p ユーザーデータへのポインタを格納するポインタ。
 * @param typetag 型タグを格納するポインタ（NULLも可）。
 * @return 対象がユーザーデータであればSQ_OK、そうでなければSQ_ERROR。
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
 * @brief ユーザーデータまたはクラスに型タグを設定します。
 * @param v 対象のSquirrel VM。
 * @param idx ユーザーデータまたはクラスのスタックインデックス。
 * @param typetag 設定する型タグ（任意のポインタ値）。
 * @return 成功した場合はSQ_OK、対象が不適切な型の場合はSQ_ERROR。
 * @details 型タグは、C側でオブジェクトの種類を安全に識別するために使用できます。
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
 * @brief HSQOBJECTから型タグを取得します。
 * @param o 対象のHSQOBJECTへのポインタ。
 * @param typetag 型タグを格納するポインタ。
 * @return 対象が型タグを持つオブジェクト（インスタンス、ユーザーデータ、クラス）であればSQ_OK、そうでなければSQ_ERROR。
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
 * @brief 指定されたインデックスのオブジェクトから型タグを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param typetag 型タグを格納するポインタ。
 * @return 対象が型タグを持つ型であればSQ_OK、そうでなければSQ_ERROR。
 * @details インスタンスの場合は、そのインスタンスが属するクラスの型タグを返します。
 */
SQRESULT sq_gettypetag(HSQUIRRELVM v,SQInteger idx,SQUserPointer *typetag)
{
    SQObjectPtr &o = stack_get(v,idx);
    if (SQ_FAILED(sq_getobjtypetag(&o, typetag)))
        return SQ_ERROR;// this is not an error it should be a bool but would break backward compatibility
    return SQ_OK;
}

/**
 * @brief 指定されたインデックスのユーザーポインタオブジェクトから、ポインタ値を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx ユーザーポインタオブジェクトのスタックインデックス。
 * @param p ポインタ値を格納するポインタ。
 * @return 対象がユーザーポインタであればSQ_OK、そうでなければSQ_ERROR。
 */
SQRESULT sq_getuserpointer(HSQUIRRELVM v, SQInteger idx, SQUserPointer *p)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_USERPOINTER,o);
    (*p) = _userpointer(*o);
    return SQ_OK;
}

/**
 * @brief クラスインスタンスにユーザーポインタを設定します。
 * @param v 対象のSquirrel VM。
 * @param idx インスタンスのスタックインデックス。
 * @param p 設定するユーザーポインタ。
 * @return 対象がインスタンスであればSQ_OK、そうでなければSQ_ERROR。
 * @details これは、C/C++側のデータをSquirrelのインスタンスに直接関連付けるための便利な方法です。
 * このポインタはSquirrelからは直接アクセスできず、C APIを介してのみ操作可能です。
 */
SQRESULT sq_setinstanceup(HSQUIRRELVM v, SQInteger idx, SQUserPointer p)
{
    SQObjectPtr &o = stack_get(v,idx);
    if(sq_type(o) != OT_INSTANCE) return sq_throwerror(v,_SC("the object is not a class instance"));
    _instance(o)->_userpointer = p;
    return SQ_OK;
}

/**
 * @brief クラスに追加のユーザーデータ領域のサイズを設定します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @param udsize 設定するユーザーデータ領域のサイズ（バイト単位）。
 * @return 成功した場合はSQ_OK、対象がクラスでないか、クラスがロックされている場合はSQ_ERROR。
 * @details この関数でサイズを設定すると、そのクラスから生成される各インスタンスは、
 * 指定されたサイズのメモリブロックを内部に持つようになります。このメモリは `sq_getinstanceup` で取得できますが、
 * この関数はインスタンスに関連付けられたポインタではなく、インスタンス内部のメモリブロックを指します。
 * クラスがロック（インスタンスが作られるなど）される前に設定する必要があります。
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
 * @brief クラスインスタンスからユーザーポインタ（またはユーザーデータ領域）を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx インスタンスのスタックインデックス。
 * @param p ユーザーポインタを格納するポインタ。
 * @param typetag 期待される型タグ。0の場合は型タグのチェックを行いません。
 * @param throwerror 型が一致しない場合にエラーをスローするかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details `typetag`が0でない場合、インスタンスのクラスまたはその基底クラスのいずれかが
 * 指定された型タグを持っているかを確認します。これにより、安全なキャストのような動作を実現できます。
 * この関数は `sq_setinstanceup` で設定されたポインタ、または `sq_setclassudsize` で確保された
 * ユーザーデータ領域へのポインタを取得します。
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
 * @brief スタック上の要素数を取得します。
 * @param v 対象のSquirrel VM。
 * @return スタック上の要素数。
 */
SQInteger sq_gettop(HSQUIRRELVM v)
{
    return (v->_top) - v->_stackbase;
}

/**
 * @brief スタックトップを指定した位置に設定します。
 * @param v 対象のSquirrel VM。
 * @param newtop 新しいスタックトップのインデックス。
 * @details `newtop`が現在のトップより小さい場合、スタックは切り詰められます（要素がポップされます）。
 * 大きい場合、スタックはnullで埋められて拡張されます。
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
 * @brief スタックトップから指定された数の要素をポップします。
 * @param v 対象のSquirrel VM。
 * @param nelemstopop ポップする要素の数。
 */
void sq_pop(HSQUIRRELVM v, SQInteger nelemstopop)
{
    assert(v->_top >= nelemstopop);
    v->Pop(nelemstopop);
}

/**
 * @brief スタックトップの要素を1つポップします。
 * @param v 対象のSquirrel VM。
 */
void sq_poptop(HSQUIRRELVM v)
{
    assert(v->_top >= 1);
    v->Pop();
}


/**
 * @brief 指定されたインデックスのスタック要素を削除します。
 * @param v 対象のSquirrel VM。
 * @param idx 削除する要素のスタックインデックス。
 * @details 削除された要素より上にある要素は下にシフトされます。
 */
void sq_remove(HSQUIRRELVM v, SQInteger idx)
{
    v->Remove(idx);
}

/**
 * @brief スタックトップの2つのオブジェクトを比較します。
 * @param v 対象のSquirrel VM。
 * @return 比較結果。`obj1 > obj2`なら1、`obj1 < obj2`なら-1、`obj1 == obj2`なら0。
 * @details この関数を呼び出す前に、比較したい2つのオブジェクトをスタックにプッシュしておく必要があります。
 * スタックトップが`obj2`、その次が`obj1`です。比較後、2つのオブジェクトはスタックに残りません。
 * この関数は内部的にSquirrelの比較演算子(`<`, `>`, `<=`, `>=`)と同じロジックを使用します。
 */
SQInteger sq_cmp(HSQUIRRELVM v)
{
    SQInteger res;
    v->ObjCmp(stack_get(v, -1), stack_get(v, -2),res);
    return res;
}

/**
 * @brief テーブルまたはクラスに新しいスロット（メンバー）を作成します。
 * @param v 対象のSquirrel VM。
 * @param idx テーブルまたはクラスのスタックインデックス。
 * @param bstatic (クラスの場合のみ) 静的メンバーとして作成するかどうか。
 * @return 常にSQ_OK。エラーはVM内で発生します。
 * @details この関数を呼び出す前に、スタックに値、その次にキーをプッシュしておく必要があります。
 * (スタック: ..., value, key)
 * 成功すると、キーと値はスタックからポップされます。
 * nullはキーとして使用できません。
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
 * @brief テーブルからスロットを削除します。
 * @param v 対象のSquirrel VM。
 * @param idx テーブルのスタックインデックス。
 * @param pushval trueの場合、削除されたスロットの値をスタックにプッシュします。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、削除するスロットのキーをスタックトップにプッシュしておく必要があります。
 * キーはスタックからポップされます。
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
 * @brief オブジェクトのスロットに値を設定します（メタメソッドを考慮します）。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、スタックに値、その次にキーをプッシュしておく必要があります。
 * (スタック: ..., value, key)
 * この関数は `_set` メタメソッドをトリガーする可能性があります。
 * 成功すると、キーと値はスタックからポップされます。
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
 * @brief オブジェクトのスロットに値を直接設定します（メタメソッドを無視します）。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、スタックに値、その次にキーをプッシュしておく必要があります。
 * (スタック: ..., value, key)
 * `_set` メタメソッドは呼び出されません。
 * 対象オブジェクトが配列、テーブル、クラス、インスタンスでない場合はエラーになります。
 * 成功すると、キーと値はスタックからポップされます。
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
 * @brief クラスに新しいメンバー（フィールドまたはメソッド）を追加します。メタメソッドを考慮します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @param bstatic 静的メンバーとして追加するかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、スタックに属性、値、キーの順でプッシュしておく必要があります。
 * (スタック: ..., attributes, value, key)
 * 属性は、メンバーに関連付けられる追加のメタデータ（通常はテーブル）です。
 * `_newmember`メタメソッドがトリガーされる可能性があります。
 * 成功すると、キー、値、属性はスタックからポップされます。
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
 * @brief クラスに新しいメンバーを直接追加します。メタメソッドを無視します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @param bstatic 静的メンバーとして追加するかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、スタックに属性、値、キーの順でプッシュしておく必要があります。
 * (スタック: ..., attributes, value, key)
 * `_newmember`メタメソッドは呼び出されません。
 * 成功すると、キー、値、属性はスタックからポップされます。
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
 * @brief テーブルまたはユーザーデータのデリゲートを設定します。
 * @param v 対象のSquirrel VM。
 * @param idx テーブルまたはユーザーデータのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、デリゲートとして設定するテーブルをスタックトップにプッシュしておく必要があります。
 * デリゲートは、元のオブジェクトにスロットが見つからない場合に検索されるフォールバックオブジェクトです。
 * スタックトップのオブジェクトがnullの場合、デリゲートは削除されます。
 * 設定後、デリゲートオブジェクトはスタックからポップされます。
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
 * @brief テーブルからスロットを直接削除します（メタメソッドを無視します）。
 * @param v 対象のSquirrel VM。
 * @param idx テーブルのスタックインデックス。
 * @param pushval trueの場合、削除されたスロットの値をスタックにプッシュします。
 * @return 常にSQ_OK。
 * @details この関数を呼び出す前に、削除するスロットのキーをスタックトップにプッシュしておく必要があります。
 * `_deleteslot`メタメソッドは呼び出されません。
 * キーに対応するスロットが存在しない場合でもエラーにはなりませんが、`pushval`がtrueの場合はnullがプッシュされます。
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
 * @brief テーブルまたはユーザーデータのデリゲートを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx テーブルまたはユーザーデータのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details デリゲートが設定されていない場合は、nullをプッシュします。
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
 * @brief オブジェクトからキーに対応する値を取得します（メタメソッドを考慮します）。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、取得したい値のキーをスタックトップにプッシュしておく必要があります。
 * 成功した場合、キーはポップされ、対応する値がスタックトップにプッシュされます。
 * この関数は `_get` メタメソッドをトリガーする可能性があります。
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
 * @brief オブジェクトからキーに対応する値を直接取得します（メタメソッドを無視します）。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、取得したい値のキーをスタックトップにプッシュしておく必要があります。
 * 成功した場合、キーはポップされ、対応する値がスタックトップにプッシュされます。
 * `_get` メタメソッドは呼び出されません。
 * 対象が配列、テーブル、クラス、インスタンスでない場合、またはキーが見つからない場合はエラーになります。
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
 * @brief 指定されたスタックインデックスのオブジェクトへのハンドル（HSQOBJECT）を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param po 結果のHSQOBJECTを格納するポインタ。
 * @return 常にSQ_OK。
 * @details このハンドルは、`sq_addref`と`sq_release`と組み合わせて使用することで、
 * オブジェクトをスタック外で安全に保持するために使用できます。
 */
SQRESULT sq_getstackobj(HSQUIRRELVM v,SQInteger idx,HSQOBJECT *po)
{
    *po=stack_get(v,idx);
    return SQ_OK;
}

/**
 * @brief 実行中の関数のローカル変数に関する情報を取得します。
 * @param v 対象のSquirrel VM。
 * @param level コールスタックのレベル（0は現在の関数、1はその呼び出し元など）。
 * @param idx ローカル変数のインデックス。
 * @return ローカル変数の名前。見つからない場合はNULL。
 * @details 成功した場合、指定されたローカル変数の値がスタックにプッシュされます。
 * この関数はデバッグ目的でのみ使用されるべきです。
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
 * @brief HSQOBJECTハンドルが指すオブジェクトをスタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param obj プッシュするオブジェクトのハンドル。
 */
void sq_pushobject(HSQUIRRELVM v,HSQOBJECT obj)
{
    v->Push(SQObjectPtr(obj));
}

/**
 * @brief HSQOBJECTハンドルをリセットしてnull状態にします。
 * @param po リセットするHSQOBJECTへのポインタ。
 * @details `sq_release`を呼び出すわけではないので注意が必要です。
 * これは単にハンドルを無効化するだけです。
 */
void sq_resetobject(HSQOBJECT *po)
{
    po->_unVal.pUserPointer=NULL;po->_type=OT_NULL;
}

/**
 * @brief VMに文字列のエラーを設定し、エラー状態にします。
 * @param v 対象のSquirrel VM。
 * @param err エラーメッセージ文字列。
 * @return 常にSQ_ERROR。
 * @details この関数は通常、API関数の実装からエラーを返すために使用されます。
 * 設定されたエラーは `sq_getlasterror` で取得できます。
 */
SQRESULT sq_throwerror(HSQUIRRELVM v,const SQChar *err)
{
    v->_lasterror=SQString::Create(_ss(v),err);
    return SQ_ERROR;
}

/**
 * @brief VMにオブジェクトのエラーを設定し、エラー状態にします。
 * @param v 対象のSquirrel VM。
 * @return 常にSQ_ERROR。
 * @details スタックトップのオブジェクトをエラーオブジェクトとして設定します。
 * 設定後、そのオブジェクトはスタックからポップされます。
 * これにより、文字列以外の任意のSquirrelオブジェクトを例外としてスローできます。
 */
SQRESULT sq_throwobject(HSQUIRRELVM v)
{
    v->_lasterror = v->GetUp(-1);
    v->Pop();
    return SQ_ERROR;
}


/**
 * @brief VMの最後のエラー状態をリセットします。
 * @param v 対象のSquirrel VM。
 * @details エラーが処理された後などに呼び出します。
 */
void sq_reseterror(HSQUIRRELVM v)
{
    v->_lasterror.Null();
}

/**
 * @brief VMに最後に設定されたエラーオブジェクトを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 */
void sq_getlasterror(HSQUIRRELVM v)
{
    v->Push(v->_lasterror);
}

/**
 * @brief VMのスタックサイズが少なくとも指定されたサイズを確保するようにします。
 * @param v 対象のSquirrel VM。
 * @param nsize 追加で必要となるスタックのスロット数。
 * @return 成功した場合はSQ_OK、失敗した（メモリ確保に失敗したか、メタメソッド実行中だった）場合はSQ_ERROR。
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
 * @brief ジェネレータの実行を再開します。
 * @param v 対象のSquirrel VM。
 * @param retval trueの場合、ジェネレータからの戻り値をスタックにプッシュします。
 * @param raiseerror 実行時エラーが発生した場合にVMにエラーを発生させるかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、再開するジェネレータオブジェクトをスタックにプッシュしておく必要があります。
 * ジェネレータに値を渡す場合（`yield`式の戻り値として）、その値をジェネレータの次にスタックにプッシュします。
 * このAPIはジェネレータの`resume`操作を模倣します。
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
 * @brief スタック上のクロージャを呼び出します。
 * @param v 対象のSquirrel VM。
 * @param params 呼び出しに渡すパラメータの数。
 * @param retval trueの場合、関数からの戻り値をスタックにプッシュします。
 * @param raiseerror 実行時エラーが発生した場合にVMにエラーを発生させるかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、呼び出されるクロージャ、その次に`this`オブジェクト（環境）、
 * そして`params`個の引数をスタックにプッシュしておく必要があります。
 * (スタック: ..., closure, this, arg1, arg2, ...)
 * 呼び出し後、クロージャと引数はスタックからポップされます。
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
 * @brief C関数からSquirrelの関数を末尾呼び出しします。
 * @param v 対象のSquirrel VM。
 * @param nparams 呼び出す関数に渡すパラメータの数。
 * @return 成功した場合はSQ_TAILCALL_FLAG。失敗した場合はSQ_ERROR。
 * @details この関数は、ネイティブ関数の中からSquirrelの関数を呼び出す際に、Cのコールスタックを消費しないようにするために使用します。
 * この関数を呼び出す前に、呼び出されるクロージャ、その次に引数をスタックにプッシュしておく必要があります。
 * (スタック: ..., closure, arg1, arg2, ...)
 * この関数が`SQ_TAILCALL_FLAG`を返した場合、呼び出し元のC関数は直ちに `SQ_TAILCALL_FLAG` をreturnしなければなりません。
 * ジェネレータは末尾呼び出しできません。
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
 * @brief VMの実行を中断します。
 * @param v 対象のSquirrel VM。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数は、C関数の中から呼び出されることを意図しており、VMをサスペンド状態にします。
 * 中断されたVMは `sq_wakeupvm` で再開できます。
 */
SQRESULT sq_suspendvm(HSQUIRRELVM v)
{
    return v->Suspend();
}

/**
 * @brief 中断されたVMの実行を再開します。
 * @param v 対象のSquirrel VM。
 * @param wakeupret trueの場合、スタックトップの値を中断した箇所への戻り値として渡します。
 * @param retval trueの場合、再開されたVMが終了または再度中断した際の戻り値をスタックにプッシュします。
 * @param raiseerror 実行時エラーが発生した場合にVMにエラーを発生させるかどうか。
 * @param throwerror trueの場合、`wakeupret`で渡された値を例外としてスローしてVMを再開します。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
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
 * @brief ユーザーデータ、インスタンス、またはクラスにリリースフックを設定します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @param hook オブジェクトがガベージコレクタによって破棄されるときに呼び出されるフック関数。
 * @details このフックは、オブジェクトに関連付けられたC/C++側のリソースを解放するために使用します。
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
 * @brief ユーザーデータ、インスタンス、またはクラスからリリースフックを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @return 設定されているリリースフック関数へのポインタ。設定されていない場合はNULL。
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
 * @brief コンパイラのエラーハンドラを設定します。
 * @param v 対象のSquirrel VM。
 * @param f 新しいコンパイラエラーハンドラ関数へのポインタ。
 * @details デフォルトでは、コンパイラエラーはVMにスローされます。
 * カスタムハンドラを設定することで、エラーの報告方法（例：ファイルへのログ出力）をカスタマイズできます。
 */
void sq_setcompilererrorhandler(HSQUIRRELVM v,SQCOMPILERERROR f)
{
    _ss(v)->_compilererrorhandler = f;
}

/**
 * @brief スクリプトクロージャをバイトコードとしてシリアライズし、指定されたライター関数に出力します。
 * @param v 対象のSquirrel VM。
 * @param w バイトコードを書き込むためのコールバック関数。
 * @param up `w`関数に渡されるユーザーポインタ。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数はスタックトップのクロージャを対象とします。
 * 自由変数を持つクロージャはシリアライズできません。
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
 * @brief シリアライズされたバイトコードを読み込み、クロージャを復元してスタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param r バイトコードを読み込むためのコールバック関数。
 * @param up `r`関数に渡されるユーザーポインタ。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
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
 * @brief VMの共有状態から一時的なメモリ領域（スクラッチパッド）を取得します。
 * @param v 対象のSquirrel VM。
 * @param minsize 必要とする最小サイズ（バイト単位）。
 * @return スクラッチパッドへのポインタ。
 * @details このメモリは、一時的な文字列操作などに使用できます。
 * 返されるポインタは、次にこの関数が呼び出されるか、他のVM操作が行われるまで有効です。
 */
SQChar *sq_getscratchpad(HSQUIRRELVM v,SQInteger minsize)
{
    return _ss(v)->GetScratchPad(minsize);
}

/**
 * @brief ガベージコレクタによって到達不可能とマークされたオブジェクトを復活させます。
 * @param v 対象のSquirrel VM。
 * @return 成功した場合はSQ_OK、GCビルドでない場合はSQ_ERROR。
 * @details この関数は、GCの「マーク」フェーズの後に呼び出されることを想定しています。
 * 到達不可能なオブジェクトの中で、リリースフックを持つものを「復活」させ、
 * ファイナライズ処理（リリースフックの呼び出し）を可能にします。
 * 通常は直接呼び出す必要はありません。
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
 * @brief インクリメンタル・ガベージコレクションのサイクルを1回実行します。
 * @param v 対象のSquirrel VM。
 * @return 回収されたオブジェクトの数。GCビルドでない場合は-1。
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
 * @brief 現在の関数の呼び出し元（callee）のクロージャを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @return 成功した場合はSQ_OK、コールスタックが浅すぎて呼び出し元が存在しない場合はSQ_ERROR。
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
 * @brief 指定されたインデックスのクロージャから自由変数を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャのスタックインデックス。
 * @param nval 取得する自由変数のインデックス（0から始まる）。
 * @return 自由変数の名前。見つからない場合や対象がクロージャでない場合はNULL。
 * @details 成功した場合、自由変数の値がスタックにプッシュされます。
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
 * @brief 指定されたインデックスのクロージャの自由変数を設定します。
 * @param v 対象のSquirrel VM。
 * @param idx クロージャのスタックインデックス。
 * @param nval 設定する自由変数のインデックス（0から始まる）。
 * @return 成功した場合はSQ_OK、失敗した（インデックスが範囲外など）場合はSQ_ERROR。
 * @details この関数を呼び出す前に、設定する値をスタックトップにプッシュしておく必要があります。
 * 設定後、その値はスタックからポップされます。
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
 * @brief クラスまたはそのメンバーの属性を設定します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、スタックに属性の値、その次にキーをプッシュしておく必要があります。
 * (スタック: ..., value, key)
 * キーがnullの場合、クラス自体の属性テーブルが設定されます。
 * キーがnullでない場合、そのキーに対応するメンバーの属性が設定されます。
 * 成功すると、古い属性値がスタックにプッシュされ、キーと値はポップされます。
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
 * @brief クラスまたはそのメンバーの属性を取得します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、属性を取得したいメンバーのキーをスタックトップにプッシュしておく必要があります。
 * キーがnullの場合、クラス自体の属性テーブルが取得されます。
 * 成功すると、キーはポップされ、属性テーブルがスタックにプッシュされます。
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
 * @brief クラスメンバーへの高速なアクセスを可能にするハンドルを取得します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @param handle 結果のメンバーハンドルを格納するポインタ。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、メンバーのキーをスタックトップにプッシュしておく必要があります。
 * 取得したハンドルは `sq_getbyhandle` や `sq_setbyhandle` と共に使用することで、
 * ハッシュテーブルのルックアップをバイパスしてメンバーに直接アクセスできます。
 * 成功するとキーはスタックからポップされます。
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
 * @brief メンバーハンドルを使用して、クラスまたはインスタンスのメンバー値へのポインタを取得します。(内部使用向け)
 * @param v 対象のSquirrel VM。
 * @param self クラスまたはインスタンスオブジェクト。
 * @param handle `sq_getmemberhandle` で取得したハンドル。
 * @param val 結果の値へのポインタを格納するポインタ。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
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
 * @brief メンバーハンドルを使用して、クラスまたはインスタンスのメンバーの値を取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx クラスまたはインスタンスのスタックインデックス。
 * @param handle `sq_getmemberhandle` で取得したハンドル。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
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
 * @brief メンバーハンドルを使用して、クラスまたはインスタンスのメンバーに値を設定します。
 * @param v 対象のSquirrel VM。
 * @param idx クラスまたはインスタンスのスタックインデックス。
 * @param handle `sq_getmemberhandle` で取得したハンドル。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details この関数を呼び出す前に、設定する値をスタックトップにプッシュしておく必要があります。
 * 設定後、その値はスタックからポップされます。
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
 * @brief クラスの基底クラスを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details 基底クラスが存在しない場合は、nullをプッシュします。
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
 * @brief インスタンスからそのクラスオブジェクトを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx インスタンスのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 */
SQRESULT sq_getclass(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_INSTANCE,o);
    v->Push(SQObjectPtr(_instance(*o)->_class));
    return SQ_OK;
}

/**
 * @brief 指定されたインデックスのクラスから新しいインスタンスを生成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx クラスのスタックインデックス。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details これはSquirrelの `class()` 構文に似ていますが、コンストラクタは呼び出しません。
 * 純粋なインスタンスの生成のみを行います。
 */
SQRESULT sq_createinstance(HSQUIRRELVM v,SQInteger idx)
{
    SQObjectPtr *o = NULL;
    _GETSAFE_OBJ(v, idx, OT_CLASS,o);
    v->Push(_class(*o)->CreateInstance());
    return SQ_OK;
}

/**
 * @brief 指定されたインデックスのオブジェクトへの弱い参照（weak reference）を作成し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx 対象オブジェクトのスタックインデックス。
 * @details 弱い参照は、オブジェクトの参照カウントを増加させません。
 * そのため、弱い参照がオブジェクトを指していても、他に強い参照がなければオブジェクトはGCによって回収されます。
 * 参照カウントされない型（整数、nullなど）に対しては、オブジェクト自身がそのままプッシュされます。
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
 * @brief 弱い参照から元のオブジェクトを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param idx 弱い参照オブジェクトのスタックインデックス。
 * @return 成功した場合はSQ_OK、対象が弱い参照でない場合はSQ_ERROR。
 * @details 元のオブジェクトが既にGCによって回収されている場合、nullがプッシュされます。
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
 * @brief 指定されたオブジェクト型のデフォルトデリゲートを取得し、スタックにプッシュします。
 * @param v 対象のSquirrel VM。
 * @param t デフォルトデリゲートを取得したいオブジェクトの型。
 * @return 成功した場合はSQ_OK、その型にデフォルトデリゲートが存在しない場合はSQ_ERROR。
 * @details デフォルトデリゲートは、特定の型のすべてのオブジェクトに共通のメソッドを提供するために使用されます。
 * 例えば、すべての文字列に共通の `slice` メソッドなどを提供します。
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
 * @brief コンテナ（テーブル、配列など）のイテレーションを1ステップ進めます。
 * @param v 対象のSquirrel VM。
 * @param idx イテレートするコンテナオブジェクトのスタックインデックス。
 * @return イテレーションが続く場合はSQ_OK、終了した場合はSQ_ERROR。
 * @details この関数は `foreach` ループをC側で実装するために使用します。
 * 呼び出す前に、イテレータ（前回 `sq_next` が返したキー、初回はnull）をスタックトップにプッシュしておく必要があります。
 * 成功した場合、イテレータはポップされ、新しいキーと値がスタックにプッシュされます。
 * (スタック: ..., new_key, new_value)
 * その後、新しいキーを次の `sq_next` 呼び出しのイテレータとして使用します。
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
 * @brief sq_compilebuffer用の内部ヘルパー構造体。
 */
struct BufState{
    const SQChar *buf;
    SQInteger ptr;
    SQInteger size;
};

/**
 * @brief sq_compilebuffer用の内部レキサーフィード関数。
 */
SQInteger buf_lexfeed(SQUserPointer file)
{
    BufState *buf=(BufState*)file;
    if(buf->size<(buf->ptr+1))
        return 0;
    return buf->buf[buf->ptr++];
}

/**
 * @brief メモリバッファ内のSquirrelソースコードをコンパイルします。
 * @param v 対象のSquirrel VM。
 * @param s ソースコードが含まれるバッファへのポインタ。
 * @param size バッファのサイズ（バイト単位）。
 * @param sourcename ソースコードの名称（デバッグ情報に使用）。
 * @param raiseerror コンパイルエラーが発生した場合にVMにエラーを発生させるかどうか。
 * @return 成功した場合はSQ_OK、失敗した場合はSQ_ERROR。
 * @details `sq_compile`のラッパーであり、ファイルI/Oの代わりにメモリバッファから直接読み込みます。
 */
SQRESULT sq_compilebuffer(HSQUIRRELVM v,const SQChar *s,SQInteger size,const SQChar *sourcename,SQBool raiseerror) {
    BufState buf;
    buf.buf = s;
    buf.size = size;
    buf.ptr = 0;
    return sq_compile(v, buf_lexfeed, &buf, sourcename, raiseerror);
}

/**
 * @brief あるVMのスタックから別のVMのスタックへオブジェクトを移動（コピー）します。
 * @param dest 移動先のVM。
 * @param src 移動元のVM。
 * @param idx 移動元のスタック上のオブジェクトのインデックス。
 */
void sq_move(HSQUIRRELVM dest,HSQUIRRELVM src,SQInteger idx)
{
    dest->Push(stack_get(src,idx));
}

/**
 * @brief VMの標準出力および標準エラー出力用のプリント関数を設定します。
 * @param v 対象のSquirrel VM。
 * @param printfunc 標準出力用の関数ポインタ。
 * @param errfunc 標準エラー出力用の関数ポインタ。
 * @details Squirrelの `print()` 関数が呼び出されたときに `printfunc` が使用され、
 * 未処理の例外が発生したときに `errfunc` が使用されます。
 */
void sq_setprintfunc(HSQUIRRELVM v, SQPRINTFUNCTION printfunc,SQPRINTFUNCTION errfunc)
{
    _ss(v)->_printfunc = printfunc;
    _ss(v)->_errorfunc = errfunc;
}

/**
 * @brief 現在設定されている標準出力用のプリント関数を取得します。
 * @param v 対象のSquirrel VM。
 * @return プリント関数のポインタ。
 */
SQPRINTFUNCTION sq_getprintfunc(HSQUIRRELVM v)
{
    return _ss(v)->_printfunc;
}

/**
 * @brief 現在設定されている標準エラー出力用のプリント関数を取得します。
 * @param v 対象のSquirrel VM。
 * @return エラープリント関数のポインタ。
 */
SQPRINTFUNCTION sq_geterrorfunc(HSQUIRRELVM v)
{
    return _ss(v)->_errorfunc;
}

/**
 * @brief Squirrelのカスタムアロケータを使用してメモリを確保します。
 * @param size 確保するサイズ（バイト単位）。
 * @return 確保されたメモリへのポインタ。
 * @details Squirrelの内部実装と同じメモリアロケータを使用したい場合に使います。
 */
void *sq_malloc(SQUnsignedInteger size)
{
    return SQ_MALLOC(size);
}

/**
 * @brief Squirrelのカスタムアロケータを使用してメモリを再確保します。
 * @param p 再確保するメモリブロックへのポインタ。
 * @param oldsize 古いサイズ（バイト単位）。
 * @param newsize 新しいサイズ（バイト単位）。
 * @return 再確保されたメモリへのポインタ。
 */
void *sq_realloc(void* p,SQUnsignedInteger oldsize,SQUnsignedInteger newsize)
{
    return SQ_REALLOC(p,oldsize,newsize);
}

/**
 * @brief Squirrelのカスタムアロケータを使用してメモリを解放します。
 * @param p 解放するメモリブロックへのポインタ。
 * @param size 解放するメモリのサイズ（バイト単位）。
 */
void sq_free(void *p,SQUnsignedInteger size)
{
    SQ_FREE(p,size);
}