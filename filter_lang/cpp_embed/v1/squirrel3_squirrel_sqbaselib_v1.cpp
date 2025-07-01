#include "sqpcheader.h"
#include "sqvm.h"
#include "sqstring.h"
#include "sqtable.h"
#include "sqarray.h"
#include "sqfuncproto.h"
#include "sqclosure.h"
#include "sqclass.h"
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

/**
 * @brief 文字列を数値（浮動小数点数または整数）に変換します。
 * @details
 * 文字列を解析し、浮動小数点数または指定された基数の整数に変換します。
 * 文字列に '.' や 'e'/'E' (科学技術表記) が含まれる場合は浮動小数点数として、
 * そうでない場合は整数として変換を試みます。
 * 13進数より大きい基数の場合、'e'は指数ではなく16進数の文字として扱われます。
 * @param [in] s 変換対象のNULL終端文字列。
 * @param [out] res 変換された数値を格納するSQObjectPtr。
 * @param [in] base 整数に変換する際の基数（2から36）。
 * @return 変換に成功した場合はtrue、失敗した場合はfalseを返します。
 */
static bool str2num(const SQChar *s,SQObjectPtr &res,SQInteger base)
{
    SQChar *end;
    const SQChar *e = s;
    bool iseintbase = base > 13; //to fix error converting hexadecimals with e like 56f0791e
    bool isfloat = false;
    SQChar c;
    while((c = *e) != _SC('\0'))
    {
        if (c == _SC('.') || (!iseintbase && (c == _SC('E') || c == _SC('e')))) { //e and E is for scientific notation
            isfloat = true;
            break;
        }
        e++;
    }
    if(isfloat){
        SQFloat r = SQFloat(scstrtod(s,&end));
        if(s == end) return false;
        res = r;
    }
    else{
        SQInteger r = SQInteger(scstrtol(s,&end,(int)base));
        if(s == end) return false;
        res = r;
    }
    return true;
}

/**
 * @brief 何も行わないダミー関数です。
 * @details
 * この関数はプレースホルダーやテスト目的で使用されることがあります。
 * 常に0を返します。
 * @param v Squirrel VMのハンドル。
 * @return 常に0。
 */
static SQInteger base_dummy(HSQUIRRELVM SQ_UNUSED_ARG(v))
{
    return 0;
}

#ifndef NO_GARBAGE_COLLECTOR
/**
 * @brief ガーベッジコレクタを実行します。
 * @details
 * 到達不可能なオブジェクトを解放し、メモリを回収します。
 * 回収されたオブジェクトの数をスタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return スタックにプッシュした値の数 (1)。
 */
static SQInteger base_collectgarbage(HSQUIRRELVM v)
{
    sq_pushinteger(v, sq_collectgarbage(v));
    return 1;
}
/**
 * @brief 到達不能オブジェクトを復活させます。
 * @details
 * ガーベッジコレクタによって到達不能と判断されたオブジェクトを、
 * 再び到達可能に戻します。主にデバッグ目的で使用されます。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。
 */
static SQInteger base_resurectureachable(HSQUIRRELVM v)
{
    sq_resurrectunreachable(v);
    return 1;
}
#endif

/**
 * @brief ルートテーブルをスタックにプッシュします。
 * @details VMの現在のルートテーブルを取得し、スタックのトップに配置します。
 * @param v Squirrel VMのハンドル。
 * @return スタックにプッシュした値の数 (1)。
 */
static SQInteger base_getroottable(HSQUIRRELVM v)
{
    v->Push(v->_roottable);
    return 1;
}

/**
 * @brief 定数テーブルをスタックにプッシュします。
 * @details VMの現在の定数テーブルを取得し、スタックのトップに配置します。
 * @param v Squirrel VMのハンドル。
 * @return スタックにプッシュした値の数 (1)。
 */
static SQInteger base_getconsttable(HSQUIRRELVM v)
{
    v->Push(_ss(v)->_consts);
    return 1;
}

/**
 * @brief ルートテーブルを設定します。
 * @details
 * スタックトップにあるテーブルを新しいルートテーブルとして設定します。
 * 変更前の古いルートテーブルをスタックにプッシュして返します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は古いルートテーブルをプッシュし1を返す。失敗した場合はSQ_ERROR。
 */
static SQInteger base_setroottable(HSQUIRRELVM v)
{
    SQObjectPtr o = v->_roottable;
    if(SQ_FAILED(sq_setroottable(v))) return SQ_ERROR;
    v->Push(o);
    return 1;
}

/**
 * @brief 定数テーブルを設定します。
 * @details
 * スタックトップにあるテーブルを新しい定数テーブルとして設定します。
 * 変更前の古い定数テーブルをスタックにプッシュして返します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は古い定数テーブルをプッシュし1を返す。失敗した場合はSQ_ERROR。
 */
static SQInteger base_setconsttable(HSQUIRRELVM v)
{
    SQObjectPtr o = _ss(v)->_consts;
    if(SQ_FAILED(sq_setconsttable(v))) return SQ_ERROR;
    v->Push(o);
    return 1;
}

/**
 * @brief 新しいエラーハンドラを設定します。
 * @details スタックトップのクロージャをVMの新しいエラーハンドラとして設定します。
 * @param v Squirrel VMのハンドル。
 * @return 常に0。
 */
static SQInteger base_seterrorhandler(HSQUIRRELVM v)
{
    sq_seterrorhandler(v);
    return 0;
}

/**
 * @brief 新しいデバッグフックを設定します。
 * @details スタックトップのクロージャをVMの新しいデバッグフックとして設定します。
 * @param v Squirrel VMのハンドル。
 * @return 常に0。
 */
static SQInteger base_setdebughook(HSQUIRRELVM v)
{
    sq_setdebughook(v);
    return 0;
}

/**
 * @brief デバッグ情報の生成を有効または無効にします。
 * @details
 * 引数（スタックの2番目）がfalseでない場合、デバッグ情報の生成を有効にします。
 * これにより、ファイル名や行番号などの情報が実行時に利用可能になります。
 * @param v Squirrel VMのハンドル。
 * @return 常に0。
 */
static SQInteger base_enabledebuginfo(HSQUIRRELVM v)
{
    SQObjectPtr &o=stack_get(v,2);

    sq_enabledebuginfo(v,SQVM::IsFalse(o)?SQFalse:SQTrue);
    return 0;
}

/**
 * @brief 指定されたレベルのコールスタック情報を取得します。
 * @details
 * 指定されたスタックレベルの情報を収集し、関数名、ソースファイル名、行番号、
 * およびローカル変数を含むテーブルをスタックに生成してプッシュします。
 * @param v Squirrel VMのハンドル。
 * @param level 情報を取得するコールスタックのレベル。
 * @return 成功した場合は1、失敗した場合は0を返します。
 */
static SQInteger __getcallstackinfos(HSQUIRRELVM v,SQInteger level)
{
    SQStackInfos si;
    SQInteger seq = 0;
    const SQChar *name = NULL;

    if (SQ_SUCCEEDED(sq_stackinfos(v, level, &si)))
    {
        const SQChar *fn = _SC("unknown");
        const SQChar *src = _SC("unknown");
        if(si.funcname)fn = si.funcname;
        if(si.source)src = si.source;
        sq_newtable(v);
        sq_pushstring(v, _SC("func"), -1);
        sq_pushstring(v, fn, -1);
        sq_newslot(v, -3, SQFalse);
        sq_pushstring(v, _SC("src"), -1);
        sq_pushstring(v, src, -1);
        sq_newslot(v, -3, SQFalse);
        sq_pushstring(v, _SC("line"), -1);
        sq_pushinteger(v, si.line);
        sq_newslot(v, -3, SQFalse);
        sq_pushstring(v, _SC("locals"), -1);
        sq_newtable(v);
        seq=0;
        while ((name = sq_getlocal(v, level, seq))) {
            sq_pushstring(v, name, -1);
            sq_push(v, -2);
            sq_newslot(v, -4, SQFalse);
            sq_pop(v, 1);
            seq++;
        }
        sq_newslot(v, -3, SQFalse);
        return 1;
    }

    return 0;
}

/**
 * @brief `getstackinfos` 関数のSquirrel APIラッパー。
 * @details
 * スタックからレベルを示す整数を取得し、`__getcallstackinfos` を呼び出して
 * スタック情報を取得し、その結果を返します。
 * @param v Squirrel VMのハンドル。
 * @return `__getcallstackinfos` の戻り値。
 */
static SQInteger base_getstackinfos(HSQUIRRELVM v)
{
    SQInteger level;
    sq_getinteger(v, -1, &level);
    return __getcallstackinfos(v,level);
}

/**
 * @brief アサーションを評価します。
 * @details
 * スタックの2番目にある値がfalseの場合、アサーション失敗エラーをスローします。
 * オプションの第2引数（スタックの3番目）をエラーメッセージとして使用できます。
 * @param v Squirrel VMのハンドル。
 * @return 条件がtrueの場合は0。falseの場合はエラーをスローするため戻りません。
 */
static SQInteger base_assert(HSQUIRRELVM v)
{
    if(SQVM::IsFalse(stack_get(v,2))){
        SQInteger top = sq_gettop(v);
        if (top>2 && SQ_SUCCEEDED(sq_tostring(v,3))) {
            const SQChar *str = 0;
            if (SQ_SUCCEEDED(sq_getstring(v,-1,&str))) {
                return sq_throwerror(v, str);
            }
        }
        return sq_throwerror(v, _SC("assertion failed"));
    }
    return 0;
}

/**
 * @brief スライス操作のパラメータを取得します。
 * @details
 * 配列や文字列のスライス操作（`slice`など）で使われる開始インデックスと
 * 終了インデックスをスタックから取得するヘルパー関数です。
 * @param v Squirrel VMのハンドル。
 * @param [out] sidx 開始インデックス。
 * @param [out] eidx 終了インデックス。
 * @param [out] o 対象のオブジェクト。
 * @return 常に1。
 */
static SQInteger get_slice_params(HSQUIRRELVM v,SQInteger &sidx,SQInteger &eidx,SQObjectPtr &o)
{
    SQInteger top = sq_gettop(v);
    sidx=0;
    eidx=0;
    o=stack_get(v,1);
    if(top>1){
        SQObjectPtr &start=stack_get(v,2);
        if(sq_type(start)!=OT_NULL && sq_isnumeric(start)){
            sidx=tointeger(start);
        }
    }
    if(top>2){
        SQObjectPtr &end=stack_get(v,3);
        if(sq_isnumeric(end)){
            eidx=tointeger(end);
        }
    }
    else {
        eidx = sq_getsize(v,1);
    }
    return 1;
}

/**
 * @brief 引数を標準出力に出力します。
 * @details
 * スタックの2番目にある値を文字列に変換し、VMに登録された
 * プリントコールバック関数 (`_printfunc`) を使って出力します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は0、失敗した場合はSQ_ERROR。
 */
static SQInteger base_print(HSQUIRRELVM v)
{
    const SQChar *str;
    if(SQ_SUCCEEDED(sq_tostring(v,2)))
    {
        if(SQ_SUCCEEDED(sq_getstring(v,-1,&str))) {
            if(_ss(v)->_printfunc) _ss(v)->_printfunc(v,_SC("%s"),str);
            return 0;
        }
    }
    return SQ_ERROR;
}

/**
 * @brief 引数をエラー出力します。
 * @details
 * スタックの2番目にある値を文字列に変換し、VMに登録された
 * エラーコールバック関数 (`_errorfunc`) を使って出力します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は0、失敗した場合はSQ_ERROR。
 */
static SQInteger base_error(HSQUIRRELVM v)
{
    const SQChar *str;
    if(SQ_SUCCEEDED(sq_tostring(v,2)))
    {
        if(SQ_SUCCEEDED(sq_getstring(v,-1,&str))) {
            if(_ss(v)->_errorfunc) _ss(v)->_errorfunc(v,_SC("%s"),str);
            return 0;
        }
    }
    return SQ_ERROR;
}

/**
 * @brief 文字列をコンパイルしてクロージャを生成します。
 * @details
 * 与えられたSquirrelコードの文字列をコンパイルし、結果として得られる
 * クロージャをスタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger base_compilestring(HSQUIRRELVM v)
{
    SQInteger nargs=sq_gettop(v);
    const SQChar *src=NULL,*name=_SC("unnamedbuffer");
    SQInteger size;
    sq_getstring(v,2,&src);
    size=sq_getsize(v,2);
    if(nargs>2){
        sq_getstring(v,3,&name);
    }
    if(SQ_SUCCEEDED(sq_compilebuffer(v,src,size,name,SQFalse)))
        return 1;
    else
        return SQ_ERROR;
}

/**
 * @brief 新しいスレッドを生成します。
 * @details
 * 引数として与えられたクロージャを開始点とする新しいVMスレッドを生成し、
 * スタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1。
 */
static SQInteger base_newthread(HSQUIRRELVM v)
{
    SQObjectPtr &func = stack_get(v,2);
    SQInteger stksize = (_closure(func)->_function->_stacksize << 1) +2;
    HSQUIRRELVM newv = sq_newthread(v, (stksize < MIN_STACK_OVERHEAD + 2)? MIN_STACK_OVERHEAD + 2 : stksize);
    sq_move(newv,v,-2);
    return 1;
}

/**
 * @brief 現在のVMを中断（サスペンド）します。
 * @details
 * VMの実行を中断し、呼び出し元に制御を返します。
 * ジェネレータやコルーチンを実現するために使用されます。
 * @param v Squirrel VMのハンドル。
 * @return sq_suspendvm の結果。
 */
static SQInteger base_suspend(HSQUIRRELVM v)
{
    return sq_suspendvm(v);
}

/**
 * @brief 新しい配列を生成します。
 * @details
 * 指定されたサイズの新しい配列を生成します。オプションで初期値を指定できます。
 * 生成した配列をスタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1。
 */
static SQInteger base_array(HSQUIRRELVM v)
{
    SQArray *a;
    SQObject &size = stack_get(v,2);
    if(sq_gettop(v) > 2) {
        a = SQArray::Create(_ss(v),0);
        a->Resize(tointeger(size),stack_get(v,3));
    }
    else {
        a = SQArray::Create(_ss(v),tointeger(size));
    }
    v->Push(a);
    return 1;
}

/**
 * @brief オブジェクトの型名を取得します。
 * @details
 * 引数として与えられたオブジェクトの型名を表す文字列を
 * スタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1。
 */
static SQInteger base_type(HSQUIRRELVM v)
{
    SQObjectPtr &o = stack_get(v,2);
    v->Push(SQString::Create(_ss(v),GetTypeName(o),-1));
    return 1;
}

/**
 * @brief 現在実行中の関数の呼び出し元クロージャを取得します。
 * @details
 * コールスタックの一つ前のレベルにあるクロージャ（呼び出し元）を取得し、
 * スタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1。コールスタックが1レベルしかない場合はエラーをスローします。
 */
static SQInteger base_callee(HSQUIRRELVM v)
{
    if(v->_callsstacksize > 1)
    {
        v->Push(v->_callsstack[v->_callsstacksize - 2]._closure);
        return 1;
    }
    return sq_throwerror(v,_SC("no closure in the calls stack"));
}

/**
 * @brief Squirrelのベースライブラリ関数を定義する静的配列。
 */
static const SQRegFunction base_funcs[]={
    //generic
    {_SC("seterrorhandler"),base_seterrorhandler,2, NULL},
    {_SC("setdebughook"),base_setdebughook,2, NULL},
    {_SC("enabledebuginfo"),base_enabledebuginfo,2, NULL},
    {_SC("getstackinfos"),base_getstackinfos,2, _SC(".n")},
    {_SC("getroottable"),base_getroottable,1, NULL},
    {_SC("setroottable"),base_setroottable,2, NULL},
    {_SC("getconsttable"),base_getconsttable,1, NULL},
    {_SC("setconsttable"),base_setconsttable,2, NULL},
    {_SC("assert"),base_assert,-2, NULL},
    {_SC("print"),base_print,2, NULL},
    {_SC("error"),base_error,2, NULL},
    {_SC("compilestring"),base_compilestring,-2, _SC(".ss")},
    {_SC("newthread"),base_newthread,2, _SC(".c")},
    {_SC("suspend"),base_suspend,-1, NULL},
    {_SC("array"),base_array,-2, _SC(".n")},
    {_SC("type"),base_type,2, NULL},
    {_SC("callee"),base_callee,0,NULL},
    {_SC("dummy"),base_dummy,0,NULL},
#ifndef NO_GARBAGE_COLLECTOR
    {_SC("collectgarbage"),base_collectgarbage,0, NULL},
    {_SC("resurrectunreachable"),base_resurectureachable,0, NULL},
#endif
    {NULL,(SQFUNCTION)0,0,NULL}
};

/**
 * @brief ベースライブラリをVMに登録します。
 * @details
 * `base_funcs` 配列に定義された全てのグローバル関数と、
 * バージョン情報などの定数をVMのルートテーブルに登録します。
 * @param v 登録対象のSquirrel VMのハンドル。
 */
void sq_base_register(HSQUIRRELVM v)
{
    SQInteger i=0;
    sq_pushroottable(v);
    while(base_funcs[i].name!=0) {
        sq_pushstring(v,base_funcs[i].name,-1);
        sq_newclosure(v,base_funcs[i].f,0);
        sq_setnativeclosurename(v,-1,base_funcs[i].name);
        sq_setparamscheck(v,base_funcs[i].nparamscheck,base_funcs[i].typemask);
        sq_newslot(v,-3, SQFalse);
        i++;
    }

    sq_pushstring(v,_SC("_versionnumber_"),-1);
    sq_pushinteger(v,SQUIRREL_VERSION_NUMBER);
    sq_newslot(v,-3, SQFalse);
    sq_pushstring(v,_SC("_version_"),-1);
    sq_pushstring(v,SQUIRREL_VERSION,-1);
    sq_newslot(v,-3, SQFalse);
    sq_pushstring(v,_SC("_charsize_"),-1);
    sq_pushinteger(v,sizeof(SQChar));
    sq_newslot(v,-3, SQFalse);
    sq_pushstring(v,_SC("_intsize_"),-1);
    sq_pushinteger(v,sizeof(SQInteger));
    sq_newslot(v,-3, SQFalse);
    sq_pushstring(v,_SC("_floatsize_"),-1);
    sq_pushinteger(v,sizeof(SQFloat));
    sq_newslot(v,-3, SQFalse);
    sq_pop(v,1);
}

/**
 * @brief オブジェクトのサイズ（長さ）を取得するデフォルトデリゲート関数。
 * @details `len()` メソッドの実装。`sq_getsize` を呼び出し、結果をスタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。
 */
static SQInteger default_delegate_len(HSQUIRRELVM v)
{
    v->Push(SQInteger(sq_getsize(v,1)));
    return 1;
}

/**
 * @brief オブジェクトを浮動小数点数に変換するデフォルトデリゲート関数。
 * @details `tofloat()` メソッドの実装。文字列、整数、浮動小数点数、真偽値を変換します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。変換できない場合はエラーをスローします。
 */
static SQInteger default_delegate_tofloat(HSQUIRRELVM v)
{
    SQObjectPtr &o=stack_get(v,1);
    switch(sq_type(o)){
    case OT_STRING:{
        SQObjectPtr res;
        if(str2num(_stringval(o),res,10)){
            v->Push(SQObjectPtr(tofloat(res)));
            break;
        }}
        return sq_throwerror(v, _SC("cannot convert the string"));
        break;
    case OT_INTEGER:case OT_FLOAT:
        v->Push(SQObjectPtr(tofloat(o)));
        break;
    case OT_BOOL:
        v->Push(SQObjectPtr((SQFloat)(_integer(o)?1:0)));
        break;
    default:
        v->PushNull();
        break;
    }
    return 1;
}

/**
 * @brief オブジェクトを整数に変換するデフォルトデリゲート関数。
 * @details `tointeger()` メソッドの実装。文字列、整数、浮動小数点数、真偽値を変換します。
 * 文字列の場合、オプションで基数を指定できます。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。変換できない場合はエラーをスローします。
 */
static SQInteger default_delegate_tointeger(HSQUIRRELVM v)
{
    SQObjectPtr &o=stack_get(v,1);
    SQInteger base = 10;
    if(sq_gettop(v) > 1) {
        sq_getinteger(v,2,&base);
    }
    switch(sq_type(o)){
    case OT_STRING:{
        SQObjectPtr res;
        if(str2num(_stringval(o),res,base)){
            v->Push(SQObjectPtr(tointeger(res)));
            break;
        }}
        return sq_throwerror(v, _SC("cannot convert the string"));
        break;
    case OT_INTEGER:case OT_FLOAT:
        v->Push(SQObjectPtr(tointeger(o)));
        break;
    case OT_BOOL:
        v->Push(SQObjectPtr(_integer(o)?(SQInteger)1:(SQInteger)0));
        break;
    default:
        v->PushNull();
        break;
    }
    return 1;
}

/**
 * @brief オブジェクトを文字列に変換するデフォルトデリゲート関数。
 * @details `tostring()` メソッドの実装。`sq_tostring` APIを呼び出して、スタック上のオブジェクトを文字列に変換します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger default_delegate_tostring(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_tostring(v,1)))
        return SQ_ERROR;
    return 1;
}

/**
 * @brief オブジェクトへの弱い参照(weak reference)を作成します。
 * @details `weakref()` メソッドの実装。ガベージコレクタがオブジェクトを回収することを妨げない参照を作成します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。
 */
static SQInteger obj_delegate_weakref(HSQUIRRELVM v)
{
    sq_weakref(v,1);
    return 1;
}

/**
 * @brief コンテナ（テーブルや配列）の内容をクリアします。
 * @details `clear()` メソッドの実装。対象コンテナのすべての要素を削除します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger obj_clear(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_clear(v,-1)) ? 1 : SQ_ERROR;
}

/**
 * @brief 数値を1文字の文字列に変換します。
 * @details `tochar()` メソッドの実装。数値を文字コードと解釈し、その文字を含む新しい文字列を生成します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。
 */
static SQInteger number_delegate_tochar(HSQUIRRELVM v)
{
    SQObject &o=stack_get(v,1);
    SQChar c = (SQChar)tointeger(o);
    v->Push(SQString::Create(_ss(v),(const SQChar *)&c,1));
    return 1;
}



/////////////////////////////////////////////////////////////////
//TABLE DEFAULT DELEGATE

/**
 * @brief テーブルからキーと値のペアをデリゲートを無視して削除します。
 * @details `rawdelete()` メソッドの実装。`_delslot` メタメソッドをトリガーせずにスロットを直接削除します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger table_rawdelete(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_rawdeleteslot(v,1,SQTrue)))
        return SQ_ERROR;
    return 1;
}

/**
 * @brief コンテナ内にキーが存在するかをデリゲートを無視して確認します。
 * @details `rawin()` メソッドの実装。`_get` メタメソッドをトリガーせずにキーの存在を直接確認します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果（true/false）をスタックにプッシュします。
 */
static SQInteger container_rawexists(HSQUIRRELVM v)
{
    if(SQ_SUCCEEDED(sq_rawget(v,-2))) {
        sq_pushbool(v,SQTrue);
        return 1;
    }
    sq_pushbool(v,SQFalse);
    return 1;
}

/**
 * @brief コンテナに値をデリゲートを無視して設定します。
 * @details `rawset()` メソッドの実装。`_set` メタメソッドをトリガーせずに値を直接設定します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger container_rawset(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_rawset(v,-3)) ? 1 : SQ_ERROR;
}

/**
 * @brief コンテナから値をデリゲートを無視して取得します。
 * @details `rawget()` メソッドの実装。`_get` メタメソッドをトリガーせずに値を直接取得します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger container_rawget(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_rawget(v,-2))?1:SQ_ERROR;
}

/**
 * @brief テーブルにデリゲートを設定します。
 * @details `setdelegate()` メソッドの実装。テーブルのデリゲート（プロトタイプ）を設定します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger table_setdelegate(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_setdelegate(v,-2)))
        return SQ_ERROR;
    sq_push(v,-1); // -1 because sq_setdelegate pops 1
    return 1;
}

/**
 * @brief テーブルのデリゲートを取得します。
 * @details `getdelegate()` メソッドの実装。テーブルの現在のデリゲートを取得し、スタックにプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger table_getdelegate(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_getdelegate(v,-1))?1:SQ_ERROR;
}

/**
 * @brief テーブルの要素をフィルタリングします。
 * @details `filter()` メソッドの実装。各キーと値のペアに対して述語関数を呼び出し、
 * その結果がtrueであったペアのみを含む新しいテーブルを生成します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果のテーブルをスタックにプッシュします。エラー発生時はSQ_ERROR。
 */
static SQInteger table_filter(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v,1);
    SQTable *tbl = _table(o);
    SQObjectPtr ret = SQTable::Create(_ss(v),0);

    SQObjectPtr itr, key, val;
    SQInteger nitr;
    while((nitr = tbl->Next(false, itr, key, val)) != -1) {
        itr = (SQInteger)nitr;

        v->Push(o);
        v->Push(key);
        v->Push(val);
        if(SQ_FAILED(sq_call(v,3,SQTrue,SQFalse))) {
            return SQ_ERROR;
        }
        if(!SQVM::IsFalse(v->GetUp(-1))) {
            _table(ret)->NewSlot(key, val);
        }
        v->Pop();
    }

    v->Push(ret);
    return 1;
}

/**
 * @brief テーブルの各要素を変換（マップ）します。
 * @details `map()` メソッドの実装。テーブルの各キーと値のペアに対して関数を呼び出し、
 * その戻り値からなる新しい配列を生成します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果の配列をスタックにプッシュします。エラー発生時はSQ_ERROR。
 */
static SQInteger table_map(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v, 1);
    SQTable *tbl = _table(o);
    SQInteger nitr, n = 0;
    SQInteger nitems = tbl->CountUsed();
    SQObjectPtr ret = SQArray::Create(_ss(v), nitems);
    SQObjectPtr itr, key, val;
    while ((nitr = tbl->Next(false, itr, key, val)) != -1) {
        itr = (SQInteger)nitr;

        v->Push(o);
        v->Push(key);
        v->Push(val);
        if (SQ_FAILED(sq_call(v, 3, SQTrue, SQFalse))) {
            return SQ_ERROR;
        }
        _array(ret)->Set(n, v->GetUp(-1));
        v->Pop();
        n++;
    }

    v->Push(ret);
    return 1;
}

/**
 * @brief テーブルのキーまたは値を配列に変換する関数を生成するマクロ。
 * @param _funcname_ 生成される関数の名前。
 * @param _valname_ 配列に格納する値（`key` または `val`）。
 */
#define TABLE_TO_ARRAY_FUNC(_funcname_,_valname_) static SQInteger _funcname_(HSQUIRRELVM v) \
{ \
    SQObject &o = stack_get(v, 1); \
    SQTable *t = _table(o); \
    SQObjectPtr itr, key, val; \
    SQObjectPtr _null; \
    SQInteger nitr, n = 0; \
    SQInteger nitems = t->CountUsed(); \
    SQArray *a = SQArray::Create(_ss(v), nitems); \
    a->Resize(nitems, _null); \
    if (nitems) { \
        while ((nitr = t->Next(false, itr, key, val)) != -1) { \
            itr = (SQInteger)nitr; \
            a->Set(n, _valname_); \
            n++; \
        } \
    } \
    v->Push(a); \
    return 1; \
}

/**
 * @brief テーブルの全てのキーを格納した新しい配列を返します。
 * @details `TABLE_TO_ARRAY_FUNC` マクロによって生成されます。
 */
TABLE_TO_ARRAY_FUNC(table_keys, key)
/**
 * @brief テーブルの全ての値を格納した新しい配列を返します。
 * @details `TABLE_TO_ARRAY_FUNC` マクロによって生成されます。
 */
TABLE_TO_ARRAY_FUNC(table_values, val)


/**
 * @brief テーブル型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_table_default_delegate_funcz[]={
    {_SC("len"),default_delegate_len,1, _SC("t")},
    {_SC("rawget"),container_rawget,2, _SC("t")},
    {_SC("rawset"),container_rawset,3, _SC("t")},
    {_SC("rawdelete"),table_rawdelete,2, _SC("t")},
    {_SC("rawin"),container_rawexists,2, _SC("t")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {_SC("clear"),obj_clear,1, _SC(".")},
    {_SC("setdelegate"),table_setdelegate,2, _SC(".t|o")},
    {_SC("getdelegate"),table_getdelegate,1, _SC(".")},
    {_SC("filter"),table_filter,2, _SC("tc")},
    {_SC("map"),table_map,2, _SC("tc") },
    {_SC("keys"),table_keys,1, _SC("t") },
    {_SC("values"),table_values,1, _SC("t") },
    {NULL,(SQFUNCTION)0,0,NULL}
};

//ARRAY DEFAULT DELEGATE///////////////////////////////////////

/**
 * @brief 配列の末尾に要素を追加します。
 * @details `append()` および `push()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger array_append(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_arrayappend(v,-2)) ? 1 : SQ_ERROR;
}

/**
 * @brief 配列に別の配列の全要素を追加します。
 * @details `extend()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。
 */
static SQInteger array_extend(HSQUIRRELVM v)
{
    _array(stack_get(v,1))->Extend(_array(stack_get(v,2)));
    sq_pop(v,1);
    return 1;
}

/**
 * @brief 配列の要素の順序を反転させます。
 * @details `reverse()` メソッドの実装。配列をインプレースで変更します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger array_reverse(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_arrayreverse(v,-1)) ? 1 : SQ_ERROR;
}

/**
 * @brief 配列の末尾の要素を削除し、その要素を返します。
 * @details `pop()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger array_pop(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_arraypop(v,1,SQTrue))?1:SQ_ERROR;
}

/**
 * @brief 配列の末尾の要素を削除せずに返します。
 * @details `top()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1。配列が空の場合はエラーをスローします。
 */
static SQInteger array_top(HSQUIRRELVM v)
{
    SQObject &o=stack_get(v,1);
    if(_array(o)->Size()>0){
        v->Push(_array(o)->Top());
        return 1;
    }
    else return sq_throwerror(v,_SC("top() on a empty array"));
}

/**
 * @brief 配列の指定したインデックスに要素を挿入します。
 * @details `insert()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。インデックスが範囲外の場合はエラーをスローします。
 */
static SQInteger array_insert(HSQUIRRELVM v)
{
    SQObject &o=stack_get(v,1);
    SQObject &idx=stack_get(v,2);
    SQObject &val=stack_get(v,3);
    if(!_array(o)->Insert(tointeger(idx),val))
        return sq_throwerror(v,_SC("index out of range"));
    sq_pop(v,2);
    return 1;
}

/**
 * @brief 配列の指定したインデックスの要素を削除し、その要素を返します。
 * @details `remove()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1。インデックスが範囲外の場合はエラーをスローします。
 */
static SQInteger array_remove(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v, 1);
    SQObject &idx = stack_get(v, 2);
    if(!sq_isnumeric(idx)) return sq_throwerror(v, _SC("wrong type"));
    SQObjectPtr val;
    if(_array(o)->Get(tointeger(idx), val)) {
        _array(o)->Remove(tointeger(idx));
        v->Push(val);
        return 1;
    }
    return sq_throwerror(v, _SC("idx out of range"));
}

/**
 * @brief 配列のサイズを変更します。
 * @details `resize()` メソッドの実装。サイズを大きくする場合、オプションで指定された値で新しい要素を埋めます。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。サイズに負の値を指定した場合などはエラーをスローします。
 */
static SQInteger array_resize(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v, 1);
    SQObject &nsize = stack_get(v, 2);
    SQObjectPtr fill;
    if(sq_isnumeric(nsize)) {
        SQInteger sz = tointeger(nsize);
        if (sz<0)
          return sq_throwerror(v, _SC("resizing to negative length"));

        if(sq_gettop(v) > 2)
            fill = stack_get(v, 3);
        _array(o)->Resize(sz,fill);
        sq_settop(v, 1);
        return 1;
    }
    return sq_throwerror(v, _SC("size must be a number"));
}

/**
 * @brief 配列の各要素に関数を適用し、結果を別の配列に格納するヘルパー関数。
 * @details `array_map` と `array_apply` の内部ロジック。
 * @param dest 結果を格納する先の配列。
 * @param src 処理対象のソース配列。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は0、失敗した場合はSQ_ERROR。
 */
static SQInteger __map_array(SQArray *dest,SQArray *src,HSQUIRRELVM v) {
    SQObjectPtr temp;
    SQInteger size = src->Size();
    SQObject &closure = stack_get(v, 2);
    v->Push(closure);

    SQInteger nArgs = 0;
    if(sq_type(closure) == OT_CLOSURE) {
        nArgs = _closure(closure)->_function->_nparameters;
    }
    else if (sq_type(closure) == OT_NATIVECLOSURE) {
        SQInteger nParamsCheck = _nativeclosure(closure)->_nparamscheck;
        if (nParamsCheck > 0)
            nArgs = nParamsCheck;
        else // push all params when there is no check or only minimal count set
            nArgs = 4;
    }

    for(SQInteger n = 0; n < size; n++) {
        src->Get(n,temp);
        v->Push(src);
        v->Push(temp);
        if (nArgs >= 3)
            v->Push(SQObjectPtr(n));
        if (nArgs >= 4)
            v->Push(src);
        if(SQ_FAILED(sq_call(v,nArgs,SQTrue,SQFalse))) {
            return SQ_ERROR;
        }
        dest->Set(n,v->GetUp(-1));
        v->Pop();
    }
    v->Pop();
    return 0;
}

/**
 * @brief 配列の各要素を変換（マップ）し、新しい配列を返します。
 * @details `map()` メソッドの実装。配列の各要素に関数を適用し、その戻り値からなる新しい配列を生成します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果の配列をスタックにプッシュします。エラー発生時はSQ_ERROR。
 */
static SQInteger array_map(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v,1);
    SQInteger size = _array(o)->Size();
    SQObjectPtr ret = SQArray::Create(_ss(v),size);
    if(SQ_FAILED(__map_array(_array(ret),_array(o),v)))
        return SQ_ERROR;
    v->Push(ret);
    return 1;
}

/**
 * @brief 配列の各要素に関数を適用し、配列をインプレースで変更します。
 * @details `apply()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。エラー発生時はSQ_ERROR。
 */
static SQInteger array_apply(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v,1);
    if(SQ_FAILED(__map_array(_array(o),_array(o),v)))
        return SQ_ERROR;
    sq_pop(v,1);
    return 1;
}

/**
 * @brief 配列の要素を単一の値に集約（リデュース）します。
 * @details `reduce()` メソッドの実装。アキュムレータと配列の各要素に対して関数を適用し、結果を累積します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。集約結果をスタックにプッシュします。エラー発生時はSQ_ERROR。
 */
static SQInteger array_reduce(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v,1);
    SQArray *a = _array(o);
    SQInteger size = a->Size();
    SQObjectPtr res;
    SQInteger iterStart;
    if (sq_gettop(v)>2) {
        res = stack_get(v,3);
        iterStart = 0;
    } else if (size==0) {
        return 0;
    } else {
        a->Get(0,res);
        iterStart = 1;
    }
    if (size > iterStart) {
        SQObjectPtr other;
        v->Push(stack_get(v,2));
        for (SQInteger n = iterStart; n < size; n++) {
            a->Get(n,other);
            v->Push(o);
            v->Push(res);
            v->Push(other);
            if(SQ_FAILED(sq_call(v,3,SQTrue,SQFalse))) {
                return SQ_ERROR;
            }
            res = v->GetUp(-1);
            v->Pop();
        }
        v->Pop();
    }
    v->Push(res);
    return 1;
}

/**
 * @brief 配列の要素をフィルタリングします。
 * @details `filter()` メソッドの実装。各要素に対して述語関数を呼び出し、
 * その結果がtrueであった要素のみを含む新しい配列を生成します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果の配列をスタックにプッシュします。エラー発生時はSQ_ERROR。
 */
static SQInteger array_filter(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v,1);
    SQArray *a = _array(o);
    SQObjectPtr ret = SQArray::Create(_ss(v),0);
    SQInteger size = a->Size();
    SQObjectPtr val;
    for(SQInteger n = 0; n < size; n++) {
        a->Get(n,val);
        v->Push(o);
        v->Push(n);
        v->Push(val);
        if(SQ_FAILED(sq_call(v,3,SQTrue,SQFalse))) {
            return SQ_ERROR;
        }
        if(!SQVM::IsFalse(v->GetUp(-1))) {
            _array(ret)->Append(val);
        }
        v->Pop();
    }
    v->Push(ret);
    return 1;
}

/**
 * @brief 配列内から指定された値を検索し、最初のインデックスを返します。
 * @details `find()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1（インデックスをスタックにプッシュ）。見つからない場合は0。
 */
static SQInteger array_find(HSQUIRRELVM v)
{
    SQObject &o = stack_get(v,1);
    SQObjectPtr &val = stack_get(v,2);
    SQArray *a = _array(o);
    SQInteger size = a->Size();
    SQObjectPtr temp;
    for(SQInteger n = 0; n < size; n++) {
        bool res = false;
        a->Get(n,temp);
        if(SQVM::IsEqual(temp,val,res) && res) {
            v->Push(n);
            return 1;
        }
    }
    return 0;
}

/**
 * @brief ソート用の比較ヘルパー関数。
 * @details
 * 2つのオブジェクトを比較します。カスタム比較関数が提供されている場合はそれを使用し、
 * そうでない場合はVMのデフォルトの比較ロジック (`ObjCmp`) を使用します。
 * ソート中に配列が変更された場合はエラーを検出します。
 * @param v Squirrel VMのハンドル。
 * @param arr ソート対象の配列。
 * @param a 比較する最初のオブジェクト。
 * @param b 比較する2番目のオブジェクト。
 * @param func カスタム比較関数のスタックインデックス。負の場合はデフォルト比較。
 * @param [out] ret 比較結果（<0, 0, >0）を格納する整数。
 * @return 比較に成功した場合はtrue、失敗した場合はfalse。
 */
static bool _sort_compare(HSQUIRRELVM v, SQArray *arr, SQObjectPtr &a,SQObjectPtr &b,SQInteger func,SQInteger &ret)
{
    if(func < 0) {
        if(!v->ObjCmp(a,b,ret)) return false;
    }
    else {
        SQInteger top = sq_gettop(v);
        sq_push(v, func);
        sq_pushroottable(v);
        v->Push(a);
        v->Push(b);
        SQObjectPtr *valptr = arr->_values._vals;
        SQUnsignedInteger precallsize = arr->_values.size();
        if(SQ_FAILED(sq_call(v, 3, SQTrue, SQFalse))) {
            if(!sq_isstring( v->_lasterror))
                v->Raise_Error(_SC("compare func failed"));
            return false;
        }
        if(SQ_FAILED(sq_getinteger(v, -1, &ret))) {
            v->Raise_Error(_SC("numeric value expected as return value of the compare function"));
            return false;
        }
        if (precallsize != arr->_values.size() || valptr != arr->_values._vals) {
            v->Raise_Error(_SC("array resized during sort operation"));
            return false;
        }
        sq_settop(v, top);
        return true;
    }
    return true;
}

/**
 * @brief ヒープソートのヘルパー関数。ヒープのプロパティを維持します。
 * @details
 * 指定されたルートノードがヒープのプロパティを満たすように、要素を下に移動させます。
 * 親ノードが子ノードより小さくなるように（または比較関数に従って）要素を交換します。
 * @param v Squirrel VMのハンドル。
 * @param arr 操作対象の配列。
 * @param root ヒープ化するサブツリーのルートノードのインデックス。
 * @param bottom ヒープの最後の要素のインデックス。
 * @param func カスタム比較関数のスタックインデックス。負の場合はデフォルト比較。
 * @return 成功した場合はtrue、失敗した場合はfalse。
 */
static bool _hsort_sift_down(HSQUIRRELVM v,SQArray *arr, SQInteger root, SQInteger bottom, SQInteger func)
{
    SQInteger maxChild;
    SQInteger done = 0;
    SQInteger ret;
    SQInteger root2;
    while (((root2 = root * 2) <= bottom) && (!done))
    {
        if (root2 == bottom) {
            maxChild = root2;
        }
        else {
            if(!_sort_compare(v,arr,arr->_values[root2],arr->_values[root2 + 1],func,ret))
                return false;
            if (ret > 0) {
                maxChild = root2;
            }
            else {
                maxChild = root2 + 1;
            }
        }

        if(!_sort_compare(v,arr,arr->_values[root],arr->_values[maxChild],func,ret))
            return false;
        if (ret < 0) {
            if (root == maxChild) {
                v->Raise_Error(_SC("inconsistent compare function"));
                return false; // We'd be swapping ourselve. The compare function is incorrect
            }

            _Swap(arr->_values[root],arr->_values[maxChild]);
            root = maxChild;
        }
        else {
            done = 1;
        }
    }
    return true;
}

/**
 * @brief ヒープソートアルゴリズムの実装。
 * @details
 * 配列をインプレースでソートします。まず配列全体をヒープ化し、
 * その後、ルート要素（最大値）を末尾に移動させ、残りの要素でヒープを再構築する、
 * という処理を繰り返します。
 * @param v Squirrel VMのハンドル。
 * @param arr ソート対象の配列オブジェクト。
 * @param l 未使用のパラメータ。
 * @param r 未使用のパラメータ。
 * @param func カスタム比較関数のスタックインデックス。負の場合はデフォルト比較。
 * @return 成功した場合はtrue、失敗した場合はfalse。
 */
static bool _hsort(HSQUIRRELVM v,SQObjectPtr &arr, SQInteger SQ_UNUSED_ARG(l), SQInteger SQ_UNUSED_ARG(r),SQInteger func)
{
    SQArray *a = _array(arr);
    SQInteger i;
    SQInteger array_size = a->Size();
    for (i = (array_size / 2); i >= 0; i--) {
        if(!_hsort_sift_down(v,a, i, array_size - 1,func)) return false;
    }

    for (i = array_size-1; i >= 1; i--)
    {
        _Swap(a->_values[0],a->_values[i]);
        if(!_hsort_sift_down(v,a, 0, i-1,func)) return false;
    }
    return true;
}

/**
 * @brief 配列をソートします。
 * @details `sort()` メソッドの実装。ヒープソートアルゴリズムを使用します。
 * オプションでカスタム比較関数を指定できます。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger array_sort(HSQUIRRELVM v)
{
    SQInteger func = -1;
    SQObjectPtr &o = stack_get(v,1);
    if(_array(o)->Size() > 1) {
        if(sq_gettop(v) == 2) func = 2;
        if(!_hsort(v, o, 0, _array(o)->Size()-1, func))
            return SQ_ERROR;

    }
    sq_settop(v,1);
    return 1;
}

/**
 * @brief 配列の一部を切り出した新しい配列（スライス）を返します。
 * @details `slice()` メソッドの実装。開始インデックスと終了インデックスを指定します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果の配列をスタックにプッシュします。インデックスが不正な場合はエラー。
 */
static SQInteger array_slice(HSQUIRRELVM v)
{
    SQInteger sidx,eidx;
    SQObjectPtr o;
    if(get_slice_params(v,sidx,eidx,o)==-1)return -1;
    SQInteger alen = _array(o)->Size();
    if(sidx < 0)sidx = alen + sidx;
    if(eidx < 0)eidx = alen + eidx;
    if(eidx < sidx)return sq_throwerror(v,_SC("wrong indexes"));
    if(eidx > alen || sidx < 0)return sq_throwerror(v, _SC("slice out of range"));
    SQArray *arr=SQArray::Create(_ss(v),eidx-sidx);
    SQObjectPtr t;
    SQInteger count=0;
    for(SQInteger i=sidx;i<eidx;i++){
        _array(o)->Get(i,t);
        arr->Set(count++,t);
    }
    v->Push(arr);
    return 1;

}

/**
 * @brief 配列型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_array_default_delegate_funcz[]={
    {_SC("len"),default_delegate_len,1, _SC("a")},
    {_SC("append"),array_append,2, _SC("a")},
    {_SC("extend"),array_extend,2, _SC("aa")},
    {_SC("push"),array_append,2, _SC("a")},
    {_SC("pop"),array_pop,1, _SC("a")},
    {_SC("top"),array_top,1, _SC("a")},
    {_SC("insert"),array_insert,3, _SC("an")},
    {_SC("remove"),array_remove,2, _SC("an")},
    {_SC("resize"),array_resize,-2, _SC("an")},
    {_SC("reverse"),array_reverse,1, _SC("a")},
    {_SC("sort"),array_sort,-1, _SC("ac")},
    {_SC("slice"),array_slice,-1, _SC("ann")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {_SC("clear"),obj_clear,1, _SC(".")},
    {_SC("map"),array_map,2, _SC("ac")},
    {_SC("apply"),array_apply,2, _SC("ac")},
    {_SC("reduce"),array_reduce,-2, _SC("ac.")},
    {_SC("filter"),array_filter,2, _SC("ac")},
    {_SC("find"),array_find,2, _SC("a.")},
    {NULL,(SQFUNCTION)0,0,NULL}
};

//STRING DEFAULT DELEGATE//////////////////////////
/**
 * @brief 文字列の一部を切り出した新しい文字列（スライス）を返します。
 * @details `slice()` メソッドの実装。開始インデックスと終了インデックスを指定します。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果の文字列をスタックにプッシュします。インデックスが不正な場合はエラー。
 */
static SQInteger string_slice(HSQUIRRELVM v)
{
    SQInteger sidx,eidx;
    SQObjectPtr o;
    if(SQ_FAILED(get_slice_params(v,sidx,eidx,o)))return -1;
    SQInteger slen = _string(o)->_len;
    if(sidx < 0)sidx = slen + sidx;
    if(eidx < 0)eidx = slen + eidx;
    if(eidx < sidx) return sq_throwerror(v,_SC("wrong indexes"));
    if(eidx > slen || sidx < 0) return sq_throwerror(v, _SC("slice out of range"));
    v->Push(SQString::Create(_ss(v),&_stringval(o)[sidx],eidx-sidx));
    return 1;
}

/**
 * @brief 文字列内から部分文字列を検索し、最初のインデックスを返します。
 * @details `find()` メソッドの実装。オプションで検索開始位置を指定できます。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1（インデックスをスタックにプッシュ）。見つからない場合は0。
 */
static SQInteger string_find(HSQUIRRELVM v)
{
    SQInteger top,start_idx=0;
    const SQChar *str,*substr,*ret;
    if(((top=sq_gettop(v))>1) && SQ_SUCCEEDED(sq_getstring(v,1,&str)) && SQ_SUCCEEDED(sq_getstring(v,2,&substr))){
        if(top>2)sq_getinteger(v,3,&start_idx);
        if((sq_getsize(v,1)>start_idx) && (start_idx>=0)){
            ret=scstrstr(&str[start_idx],substr);
            if(ret){
                sq_pushinteger(v,(SQInteger)(ret-str));
                return 1;
            }
        }
        return 0;
    }
    return sq_throwerror(v,_SC("invalid param"));
}

/**
 * @brief 文字列の各文字に関数を適用する関数を生成するマクロ。
 * @details tolower, toupper のような文字単位の変換処理を実装するために使用します。
 * @param func 適用する文字変換関数 (例: tolower)。
 */
#define STRING_TOFUNCZ(func) static SQInteger string_##func(HSQUIRRELVM v) \
{\
    SQInteger sidx,eidx; \
    SQObjectPtr str; \
    if(SQ_FAILED(get_slice_params(v,sidx,eidx,str)))return -1; \
    SQInteger slen = _string(str)->_len; \
    if(sidx < 0)sidx = slen + sidx; \
    if(eidx < 0)eidx = slen + eidx; \
    if(eidx < sidx) return sq_throwerror(v,_SC("wrong indexes")); \
    if(eidx > slen || sidx < 0) return sq_throwerror(v,_SC("slice out of range")); \
    SQInteger len=_string(str)->_len; \
    const SQChar *sthis=_stringval(str); \
    SQChar *snew=(_ss(v)->GetScratchPad(sq_rsl(len))); \
    memcpy(snew,sthis,sq_rsl(len));\
    for(SQInteger i=sidx;i<eidx;i++) snew[i] = func(sthis[i]); \
    v->Push(SQString::Create(_ss(v),snew,len)); \
    return 1; \
}

/**
 * @brief 文字列を小文字に変換した新しい文字列を返します。
 * @details `tolower()` メソッドの実装。`STRING_TOFUNCZ` マクロによって生成されます。
 */
STRING_TOFUNCZ(tolower)
/**
 * @brief 文字列を大文字に変換した新しい文字列を返します。
 * @details `toupper()` メソッドの実装。`STRING_TOFUNCZ` マクロによって生成されます。
 */
STRING_TOFUNCZ(toupper)

/**
 * @brief 文字列型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_string_default_delegate_funcz[]={
    {_SC("len"),default_delegate_len,1, _SC("s")},
    {_SC("tointeger"),default_delegate_tointeger,-1, _SC("sn")},
    {_SC("tofloat"),default_delegate_tofloat,1, _SC("s")},
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {_SC("slice"),string_slice,-1, _SC("s n  n")},
    {_SC("find"),string_find,-2, _SC("s s n")},
    {_SC("tolower"),string_tolower,-1, _SC("s n n")},
    {_SC("toupper"),string_toupper,-1, _SC("s n n")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {NULL,(SQFUNCTION)0,0,NULL}
};

/**
 * @brief 数値型（整数・浮動小数点数）のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_number_default_delegate_funcz[]={
    {_SC("tointeger"),default_delegate_tointeger,1, _SC("n|b")},
    {_SC("tofloat"),default_delegate_tofloat,1, _SC("n|b")},
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {_SC("tochar"),number_delegate_tochar,1, _SC("n|b")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {NULL,(SQFUNCTION)0,0,NULL}
};

//CLOSURE DEFAULT DELEGATE//////////////////////////
/**
 * @brief クロージャを保護付きで呼び出します。
 * @details `pcall()` メソッドの実装。呼び出し中にエラーが発生してもスクリプトを停止せず、エラーオブジェクトを返します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger closure_pcall(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_call(v,sq_gettop(v)-1,SQTrue,SQFalse))?1:SQ_ERROR;
}

/**
 * @brief クロージャを呼び出します。
 * @details `call()` メソッドの実装。可能であれば末尾呼び出し最適化を行います。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger closure_call(HSQUIRRELVM v)
{
    SQObjectPtr &c = stack_get(v, -1);
    if (sq_type(c) == OT_CLOSURE && (_closure(c)->_function->_bgenerator == false))
    {
        return sq_tailcall(v, sq_gettop(v) - 1);
    }
    return SQ_SUCCEEDED(sq_call(v, sq_gettop(v) - 1, SQTrue, SQTrue)) ? 1 : SQ_ERROR;
}

/**
 * @brief 配列を引数リストとしてクロージャを呼び出すヘルパー関数。
 * @details `acall` と `pacall` の内部実装。
 * @param v Squirrel VMのハンドル。
 * @param raiseerror エラー発生時にVMエラーを発生させるかどうかのフラグ。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger _closure_acall(HSQUIRRELVM v,SQBool raiseerror)
{
    SQArray *aparams=_array(stack_get(v,2));
    SQInteger nparams=aparams->Size();
    v->Push(stack_get(v,1));
    for(SQInteger i=0;i<nparams;i++)v->Push(aparams->_values[i]);
    return SQ_SUCCEEDED(sq_call(v,nparams,SQTrue,raiseerror))?1:SQ_ERROR;
}

/**
 * @brief 配列を引数リストとしてクロージャを呼び出します。
 * @details `acall()` メソッドの実装。エラーが発生するとVMエラーを発生させます。
 * @param v Squirrel VMのハンドル。
 * @return `_closure_acall` の戻り値。
 */
static SQInteger closure_acall(HSQUIRRELVM v)
{
    return _closure_acall(v,SQTrue);
}

/**
 * @brief 配列を引数リストとしてクロージャを保護付きで呼び出します。
 * @details `pacall()` メソッドの実装。エラーが発生してもVMエラーを発生させません。
 * @param v Squirrel VMのハンドル。
 * @return `_closure_acall` の戻り値。
 */
static SQInteger closure_pacall(HSQUIRRELVM v)
{
    return _closure_acall(v,SQFalse);
}

/**
 * @brief クロージャに環境オブジェクトを束縛します。
 * @details `bindenv()` メソッドの実装。クロージャの自由変数の解決に使われる環境（テーブルやクラスインスタンス）を設定します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger closure_bindenv(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_bindenv(v,1)))
        return SQ_ERROR;
    return 1;
}

/**
 * @brief クロージャのルートテーブルを取得します。
 * @details `getroot()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger closure_getroot(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_getclosureroot(v,-1)))
        return SQ_ERROR;
    return 1;
}

/**
 * @brief クロージャのルートテーブルを設定します。
 * @details `setroot()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger closure_setroot(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_setclosureroot(v,-2)))
        return SQ_ERROR;
    return 1;
}

/**
 * @brief クロージャに関する情報を取得します。
 * @details `getinfos()` メソッドの実装。関数名、ソース名、パラメータ、デフォルトパラメータなどの情報を
 * 格納したテーブルを生成して返します。ネイティブクロージャとSquirrelクロージャの両方に対応しています。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。結果のテーブルをスタックにプッシュします。
 */
static SQInteger closure_getinfos(HSQUIRRELVM v) {
    SQObject o = stack_get(v,1);
    SQTable *res = SQTable::Create(_ss(v),4);
    if(sq_type(o) == OT_CLOSURE) {
        SQFunctionProto *f = _closure(o)->_function;
        SQInteger nparams = f->_nparameters + (f->_varparams?1:0);
        SQObjectPtr params = SQArray::Create(_ss(v),nparams);
    SQObjectPtr defparams = SQArray::Create(_ss(v),f->_ndefaultparams);
        for(SQInteger n = 0; n<f->_nparameters; n++) {
            _array(params)->Set((SQInteger)n,f->_parameters[n]);
        }
    for(SQInteger j = 0; j<f->_ndefaultparams; j++) {
            _array(defparams)->Set((SQInteger)j,_closure(o)->_defaultparams[j]);
        }
        if(f->_varparams) {
            _array(params)->Set(nparams-1,SQString::Create(_ss(v),_SC("..."),-1));
        }
        res->NewSlot(SQString::Create(_ss(v),_SC("native"),-1),false);
        res->NewSlot(SQString::Create(_ss(v),_SC("name"),-1),f->_name);
        res->NewSlot(SQString::Create(_ss(v),_SC("src"),-1),f->_sourcename);
        res->NewSlot(SQString::Create(_ss(v),_SC("parameters"),-1),params);
        res->NewSlot(SQString::Create(_ss(v),_SC("varargs"),-1),f->_varparams);
    res->NewSlot(SQString::Create(_ss(v),_SC("defparams"),-1),defparams);
    }
    else { //OT_NATIVECLOSURE
        SQNativeClosure *nc = _nativeclosure(o);
        res->NewSlot(SQString::Create(_ss(v),_SC("native"),-1),true);
        res->NewSlot(SQString::Create(_ss(v),_SC("name"),-1),nc->_name);
        res->NewSlot(SQString::Create(_ss(v),_SC("paramscheck"),-1),nc->_nparamscheck);
        SQObjectPtr typecheck;
        if(nc->_typecheck.size() > 0) {
            typecheck =
                SQArray::Create(_ss(v), nc->_typecheck.size());
            for(SQUnsignedInteger n = 0; n<nc->_typecheck.size(); n++) {
                    _array(typecheck)->Set((SQInteger)n,nc->_typecheck[n]);
            }
        }
        res->NewSlot(SQString::Create(_ss(v),_SC("typecheck"),-1),typecheck);
    }
    v->Push(res);
    return 1;
}

/**
 * @brief クロージャ型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_closure_default_delegate_funcz[]={
    {_SC("call"),closure_call,-1, _SC("c")},
    {_SC("pcall"),closure_pcall,-1, _SC("c")},
    {_SC("acall"),closure_acall,2, _SC("ca")},
    {_SC("pacall"),closure_pacall,2, _SC("ca")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {_SC("bindenv"),closure_bindenv,2, _SC("c x|y|t")},
    {_SC("getinfos"),closure_getinfos,1, _SC("c")},
    {_SC("getroot"),closure_getroot,1, _SC("c")},
    {_SC("setroot"),closure_setroot,2, _SC("ct")},
    {NULL,(SQFUNCTION)0,0,NULL}
};

//GENERATOR DEFAULT DELEGATE
/**
 * @brief ジェネレータの現在の状態を取得します。
 * @details `getstatus()` メソッドの実装。状態は "suspended", "running", "dead" のいずれかです。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。状態を表す文字列をスタックにプッシュします。
 */
static SQInteger generator_getstatus(HSQUIRRELVM v)
{
    SQObject &o=stack_get(v,1);
    switch(_generator(o)->_state){
        case SQGenerator::eSuspended:v->Push(SQString::Create(_ss(v),_SC("suspended")));break;
        case SQGenerator::eRunning:v->Push(SQString::Create(_ss(v),_SC("running")));break;
        case SQGenerator::eDead:v->Push(SQString::Create(_ss(v),_SC("dead")));break;
    }
    return 1;
}

/**
 * @brief ジェネレータ型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_generator_default_delegate_funcz[]={
    {_SC("getstatus"),generator_getstatus,1, _SC("g")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {NULL,(SQFUNCTION)0,0,NULL}
};

//THREAD DEFAULT DELEGATE
/**
 * @brief スレッドを開始または再開します。
 * @details `call()` メソッドの実装。スレッドのルートクロージャを呼び出します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger thread_call(HSQUIRRELVM v)
{
    SQObjectPtr o = stack_get(v,1);
    if(sq_type(o) == OT_THREAD) {
        SQInteger nparams = sq_gettop(v);
        _thread(o)->Push(_thread(o)->_roottable);
        for(SQInteger i = 2; i<(nparams+1); i++)
            sq_move(_thread(o),v,i);
        if(SQ_SUCCEEDED(sq_call(_thread(o),nparams,SQTrue,SQTrue))) {
            sq_move(v,_thread(o),-1);
            sq_pop(_thread(o),1);
            return 1;
        }
        v->_lasterror = _thread(o)->_lasterror;
        return SQ_ERROR;
    }
    return sq_throwerror(v,_SC("wrong parameter"));
}

/**
 * @brief 中断状態のスレッドを再開します。
 * @details `wakeup()` メソッドの実装。オプションで値をスレッドに渡すことができます。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger thread_wakeup(HSQUIRRELVM v)
{
    SQObjectPtr o = stack_get(v,1);
    if(sq_type(o) == OT_THREAD) {
        SQVM *thread = _thread(o);
        SQInteger state = sq_getvmstate(thread);
        if(state != SQ_VMSTATE_SUSPENDED) {
            switch(state) {
                case SQ_VMSTATE_IDLE:
                    return sq_throwerror(v,_SC("cannot wakeup a idle thread"));
                break;
                case SQ_VMSTATE_RUNNING:
                    return sq_throwerror(v,_SC("cannot wakeup a running thread"));
                break;
            }
        }

        SQInteger wakeupret = sq_gettop(v)>1?SQTrue:SQFalse;
        if(wakeupret) {
            sq_move(thread,v,2);
        }
        if(SQ_SUCCEEDED(sq_wakeupvm(thread,wakeupret,SQTrue,SQTrue,SQFalse))) {
            sq_move(v,thread,-1);
            sq_pop(thread,1); //pop retval
            if(sq_getvmstate(thread) == SQ_VMSTATE_IDLE) {
                sq_settop(thread,1); //pop roottable
            }
            return 1;
        }
        sq_settop(thread,1);
        v->_lasterror = thread->_lasterror;
        return SQ_ERROR;
    }
    return sq_throwerror(v,_SC("wrong parameter"));
}

/**
 * @brief 中断状態のスレッドに例外をスローして再開させます。
 * @details `wakeupthrow()` メソッドの実装。指定されたオブジェクトをスレッド内でスローして実行を再開します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger thread_wakeupthrow(HSQUIRRELVM v)
{
    SQObjectPtr o = stack_get(v,1);
    if(sq_type(o) == OT_THREAD) {
        SQVM *thread = _thread(o);
        SQInteger state = sq_getvmstate(thread);
        if(state != SQ_VMSTATE_SUSPENDED) {
            switch(state) {
                case SQ_VMSTATE_IDLE:
                    return sq_throwerror(v,_SC("cannot wakeup a idle thread"));
                break;
                case SQ_VMSTATE_RUNNING:
                    return sq_throwerror(v,_SC("cannot wakeup a running thread"));
                break;
            }
        }

        sq_move(thread,v,2);
        sq_throwobject(thread);
        SQBool rethrow_error = SQTrue;
        if(sq_gettop(v) > 2) {
            sq_getbool(v,3,&rethrow_error);
        }
        if(SQ_SUCCEEDED(sq_wakeupvm(thread,SQFalse,SQTrue,SQTrue,SQTrue))) {
            sq_move(v,thread,-1);
            sq_pop(thread,1); //pop retval
            if(sq_getvmstate(thread) == SQ_VMSTATE_IDLE) {
                sq_settop(thread,1); //pop roottable
            }
            return 1;
        }
        sq_settop(thread,1);
        if(rethrow_error) {
            v->_lasterror = thread->_lasterror;
            return SQ_ERROR;
        }
        return SQ_OK;
    }
    return sq_throwerror(v,_SC("wrong parameter"));
}

/**
 * @brief スレッドの現在の状態を取得します。
 * @details `getstatus()` メソッドの実装。状態は "idle", "running", "suspended" のいずれかです。
 * @param v Squirrel VMのハンドル。
 * @return 常に1。状態を表す文字列をスタックにプッシュします。
 */
static SQInteger thread_getstatus(HSQUIRRELVM v)
{
    SQObjectPtr &o = stack_get(v,1);
    switch(sq_getvmstate(_thread(o))) {
        case SQ_VMSTATE_IDLE:
            sq_pushstring(v,_SC("idle"),-1);
        break;
        case SQ_VMSTATE_RUNNING:
            sq_pushstring(v,_SC("running"),-1);
        break;
        case SQ_VMSTATE_SUSPENDED:
            sq_pushstring(v,_SC("suspended"),-1);
        break;
        default:
            return sq_throwerror(v,_SC("internal VM error"));
    }
    return 1;
}

/**
 * @brief 指定されたスレッドのコールスタック情報を取得します。
 * @details `getstackinfos()` メソッドの実装。対象スレッドのスタック情報をテーブルとして返します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERRORまたは0。
 */
static SQInteger thread_getstackinfos(HSQUIRRELVM v)
{
    SQObjectPtr o = stack_get(v,1);
    if(sq_type(o) == OT_THREAD) {
        SQVM *thread = _thread(o);
        SQInteger threadtop = sq_gettop(thread);
        SQInteger level;
        sq_getinteger(v,-1,&level);
        SQRESULT res = __getcallstackinfos(thread,level);
        if(SQ_FAILED(res))
        {
            sq_settop(thread,threadtop);
            if(sq_type(thread->_lasterror) == OT_STRING) {
                sq_throwerror(v,_stringval(thread->_lasterror));
            }
            else {
                sq_throwerror(v,_SC("unknown error"));
            }
        }
        if(res > 0) {
            //some result
            sq_move(v,thread,-1);
            sq_settop(thread,threadtop);
            return 1;
        }
        //no result
        sq_settop(thread,threadtop);
        return 0;

    }
    return sq_throwerror(v,_SC("wrong parameter"));
}

/**
 * @brief スレッド型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_thread_default_delegate_funcz[] = {
    {_SC("call"), thread_call, -1, _SC("v")},
    {_SC("wakeup"), thread_wakeup, -1, _SC("v")},
    {_SC("wakeupthrow"), thread_wakeupthrow, -2, _SC("v.b")},
    {_SC("getstatus"), thread_getstatus, 1, _SC("v")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("getstackinfos"),thread_getstackinfos,2, _SC("vn")},
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {NULL,(SQFUNCTION)0,0,NULL}
};

/**
 * @brief クラスメンバの属性を取得します。
 * @details `getattributes()` メソッドの実装。指定されたキー（メンバ名）に対応する属性テーブルを取得します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger class_getattributes(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_getattributes(v,-2))?1:SQ_ERROR;
}

/**
 * @brief クラスメンバの属性を設定します。
 * @details `setattributes()` メソッドの実装。指定されたキー（メンバ名）に新しい属性テーブルを設定します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger class_setattributes(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_setattributes(v,-3))?1:SQ_ERROR;
}

/**
 * @brief クラスの新しいインスタンスを生成します。
 * @details `instance()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger class_instance(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_createinstance(v,-1))?1:SQ_ERROR;
}

/**
 * @brief クラスの基底クラスを取得します。
 * @details `getbase()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger class_getbase(HSQUIRRELVM v)
{
    return SQ_SUCCEEDED(sq_getbase(v,-1))?1:SQ_ERROR;
}

/**
 * @brief クラスに新しいメンバを追加します。
 * @details `newmember()` メソッドの実装。`_newmember` メタメソッドをトリガーする可能性があります。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger class_newmember(HSQUIRRELVM v)
{
    SQInteger top = sq_gettop(v);
    SQBool bstatic = SQFalse;
    if(top == 5)
    {
        sq_tobool(v,-1,&bstatic);
        sq_pop(v,1);
    }

    if(top < 4) {
        sq_pushnull(v);
    }
    return SQ_SUCCEEDED(sq_newmember(v,-4,bstatic))?1:SQ_ERROR;
}

/**
 * @brief クラスに新しいメンバをメタメソッドを無視して追加します。
 * @details `rawnewmember()` メソッドの実装。メタメソッドをトリガーせずにメンバを直接追加します。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger class_rawnewmember(HSQUIRRELVM v)
{
    SQInteger top = sq_gettop(v);
    SQBool bstatic = SQFalse;
    if(top == 5)
    {
        sq_tobool(v,-1,&bstatic);
        sq_pop(v,1);
    }

    if(top < 4) {
        sq_pushnull(v);
    }
    return SQ_SUCCEEDED(sq_rawnewmember(v,-4,bstatic))?1:SQ_ERROR;
}

/**
 * @brief クラス型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_class_default_delegate_funcz[] = {
    {_SC("getattributes"), class_getattributes, 2, _SC("y.")},
    {_SC("setattributes"), class_setattributes, 3, _SC("y..")},
    {_SC("rawget"),container_rawget,2, _SC("y")},
    {_SC("rawset"),container_rawset,3, _SC("y")},
    {_SC("rawin"),container_rawexists,2, _SC("y")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {_SC("instance"),class_instance,1, _SC("y")},
    {_SC("getbase"),class_getbase,1, _SC("y")},
    {_SC("newmember"),class_newmember,-3, _SC("y")},
    {_SC("rawnewmember"),class_rawnewmember,-3, _SC("y")},
    {NULL,(SQFUNCTION)0,0,NULL}
};

/**
 * @brief インスタンスのクラスを取得します。
 * @details `getclass()` メソッドの実装。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger instance_getclass(HSQUIRRELVM v)
{
    if(SQ_SUCCEEDED(sq_getclass(v,1)))
        return 1;
    return SQ_ERROR;
}

/**
 * @brief インスタンス型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_instance_default_delegate_funcz[] = {
    {_SC("getclass"), instance_getclass, 1, _SC("x")},
    {_SC("rawget"),container_rawget,2, _SC("x")},
    {_SC("rawset"),container_rawset,3, _SC("x")},
    {_SC("rawin"),container_rawexists,2, _SC("x")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {NULL,(SQFUNCTION)0,0,NULL}
};

/**
 * @brief 弱い参照(weak reference)から元のオブジェクトを取得します。
 * @details `ref()` メソッドの実装。弱い参照が指すオブジェクトをスタックにプッシュします。オブジェクトが既に解放済みの場合はnullをプッシュします。
 * @param v Squirrel VMのハンドル。
 * @return 成功した場合は1、失敗した場合はSQ_ERROR。
 */
static SQInteger weakref_ref(HSQUIRRELVM v)
{
    if(SQ_FAILED(sq_getweakrefval(v,1)))
        return SQ_ERROR;
    return 1;
}

/**
 * @brief 弱い参照(weak reference)型のデフォルトデリゲートに登録される関数群。
 */
const SQRegFunction SQSharedState::_weakref_default_delegate_funcz[] = {
    {_SC("ref"),weakref_ref,1, _SC("r")},
    {_SC("weakref"),obj_delegate_weakref,1, NULL },
    {_SC("tostring"),default_delegate_tostring,1, _SC(".")},
    {NULL,(SQFUNCTION)0,0,NULL}
};