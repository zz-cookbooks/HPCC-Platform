/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "platform.h"
#include "v8.h"
#include "jexcept.hpp"
#include "jthread.hpp"
#include "deftype.hpp"
#include "eclrtl.hpp"
#include "eclrtl_imp.hpp"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

namespace javascriptLanguageHelper {

class V8JavascriptEmbedFunctionContext : public CInterfaceOf<IEmbedFunctionContext>
{
public:
    V8JavascriptEmbedFunctionContext()
    {
        isolate = v8::Isolate::New();
        isolate->Enter();
        context = v8::Context::New();
        context->Enter();
    }
    ~V8JavascriptEmbedFunctionContext()
    {
        script.Dispose();
        result.Dispose();
        context->Exit();
        context.Dispose();
        isolate->Exit();
        isolate->Dispose();
    }

    virtual void bindBooleanParam(const char *name, bool val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Boolean::New(val));
    }
    virtual void bindDataParam(const char *name, size32_t len, const void *val)
    {
        v8::HandleScope handle_scope;
        v8::Local<v8::Array> array = v8::Array::New(len);
        const byte *vval = (const byte *) val;
        for (int i = 0; i < len; i++)
        {
            array->Set(v8::Number::New(i), v8::Integer::New(vval[i])); // feels horridly inefficient, but seems to be the expected approach
        }
        context->Global()->Set(v8::String::New(name), array);
    }
    virtual void bindRealParam(const char *name, double val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Number::New(val));
    }
    virtual void bindSignedParam(const char *name, __int64 val)
    {
        // MORE - might need to check does not overflow 32 bits? Or store as a real?
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Integer::New(val));
    }
    virtual void bindUnsignedParam(const char *name, unsigned __int64 val)
    {
        // MORE - might need to check does not overflow 32 bits
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Integer::NewFromUnsigned(val));
    }
    virtual void bindStringParam(const char *name, size32_t len, const char *val)
    {
        size32_t utfCharCount;
        rtlDataAttr utfText;
        rtlStrToUtf8X(utfCharCount, utfText.refstr(), len, val);
        bindUTF8Param(name, utfCharCount, utfText.getstr());
    }
    virtual void bindVStringParam(const char *name, const char *val)
    {
        bindStringParam(name, strlen(val), val);
    }
    virtual void bindUTF8Param(const char *name, size32_t chars, const char *val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::String::New(val, rtlUtf8Size(chars, val)));
    }
    virtual void bindUnicodeParam(const char *name, size32_t chars, const UChar *val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::String::New(val, chars));
    }
    virtual void bindSetParam(const char *name, int elemType, size32_t elemSize, bool isAll, size32_t totalBytes, void *setData)
    {
        if (isAll)
            rtlFail(0, "v8embed: Cannot pass ALL");
        v8::HandleScope handle_scope;
        type_t typecode = (type_t) elemType;
        const byte *inData = (const byte *) setData;
        const byte *endData = inData + totalBytes;
        int numElems;
        if (elemSize == UNKNOWN_LENGTH)
        {
            numElems = 0;
            // Will need 2 passes to work out how many elements there are in the set :(
            while (inData < endData)
            {
                int thisSize;
                switch (elemType)
                {
                case type_varstring:
                    thisSize = strlen((const char *) inData) + 1;
                    break;
                case type_string:
                    thisSize = * (size32_t *) inData + sizeof(size32_t);
                    break;
                case type_unicode:
                    thisSize = (* (size32_t *) inData) * sizeof(UChar) + sizeof(size32_t);
                    break;
                case type_utf8:
                    thisSize = rtlUtf8Size(* (size32_t *) inData, inData + sizeof(size32_t)) + sizeof(size32_t);
                    break;
                default:
                    rtlFail(0, "v8embed: Unsupported parameter type");
                    break;
                }
                inData += thisSize;
                numElems++;
            }
            inData = (const byte *) setData;
        }
        else
            numElems = totalBytes / elemSize;
        v8::Local<v8::Array> array = v8::Array::New(numElems);
        v8::Handle<v8::Value> thisItem;
        size32_t thisSize = elemSize;
        for (int idx = 0; idx < numElems; idx++)
        {
            switch (typecode)
            {
            case type_int:
                thisItem = v8::Integer::New(rtlReadInt(inData, elemSize));
                break;
            case type_unsigned:
                thisItem = v8::Integer::NewFromUnsigned(rtlReadUInt(inData, elemSize));
                break;
            case type_varstring:
            {
                size32_t numChars = strlen((const char *) inData);
                size32_t utfCharCount;
                rtlDataAttr utfText;
                rtlStrToUtf8X(utfCharCount, utfText.refstr(), numChars, (const char *) inData);
                thisItem = v8::String::New(utfText.getstr(), rtlUtf8Size(utfCharCount, utfText.getstr()));
                if (elemSize == UNKNOWN_LENGTH)
                    thisSize = numChars + 1;
                break;
            }
            case type_string:
            {
                if (elemSize == UNKNOWN_LENGTH)
                {
                    thisSize = * (size32_t *) inData;
                    inData += sizeof(size32_t);
                }
                size32_t utfCharCount;
                rtlDataAttr utfText;
                rtlStrToUtf8X(utfCharCount, utfText.refstr(), thisSize, (const char *) inData);
                thisItem = v8::String::New(utfText.getstr(), rtlUtf8Size(utfCharCount, utfText.getstr()));
                break;
            }
            case type_real:
                if (elemSize == sizeof(double))
                    thisItem = v8::Number::New(* (double *) inData);
                else
                    thisItem = v8::Number::New(* (float *) inData);
                break;
            case type_boolean:
                assertex(elemSize == sizeof(bool));
                thisItem = v8::Boolean::New(* (bool *) inData);
                break;
            case type_unicode:
            {
                if (elemSize == UNKNOWN_LENGTH)
                {
                    thisSize = (* (size32_t *) inData) * sizeof(UChar); // NOTE - it's in chars...
                    inData += sizeof(size32_t);
                }
                thisItem = v8::String::New((const UChar *) inData, thisSize/sizeof(UChar));
                break;
            }
            case type_utf8:
            {
                assertex (elemSize == UNKNOWN_LENGTH);
                size32_t numChars = * (size32_t *) inData;
                inData += sizeof(size32_t);
                thisSize = rtlUtf8Size(numChars, inData);
                thisItem = v8::String::New((const char *) inData, thisSize);
                break;
            }
            default:
                rtlFail(0, "v8embed: Unsupported parameter type");
                break;
            }
            inData += thisSize;
            array->Set(v8::Number::New(idx), thisItem);
        }
        context->Global()->Set(v8::String::New(name), array);
    }

    virtual bool getBooleanResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return result->BooleanValue();
    }
    virtual void getDataResult(size32_t &__len, void * &__result)
    {
        assertex (!result.IsEmpty() && result->IsArray());
        v8::HandleScope handle_scope;
        v8::Handle<v8::Array> array = v8::Handle<v8::Array>::Cast(result);
        __len = array->Length();
        __result = rtlMalloc(__len);
        byte *bresult = (byte *) __result;
        for (size32_t i = 0; i < __len; i++)
        {
            bresult[i] = v8::Integer::Cast(*array->Get(i))->Value(); // feels horridly inefficient, but seems to be the expected approach
        }
    }
    virtual double getRealResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return v8::Number::Cast(*result)->Value();
    }
    virtual __int64 getSignedResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return v8::Integer::Cast(*result)->Value();
    }
    virtual unsigned __int64 getUnsignedResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return v8::Integer::Cast(*result)->Value();
    }
    virtual void getStringResult(size32_t &__chars, char * &__result)
    {
        assertex (!result.IsEmpty() && result->IsString());
        v8::HandleScope handle_scope;
        v8::String::AsciiValue ascii(result);
        rtlStrToStrX(__chars, __result, ascii.length(), *ascii);
    }
    virtual void getUTF8Result(size32_t &__chars, char * &__result)
    {
        assertex (!result.IsEmpty() && result->IsString());
        v8::HandleScope handle_scope;
        v8::String::Utf8Value utf8(result);
        unsigned numchars = rtlUtf8Length(utf8.length(), *utf8);
        rtlUtf8ToUtf8X(__chars, __result, numchars, *utf8);
    }
    virtual void getUnicodeResult(size32_t &__chars, UChar * &__result)
    {
        assertex (!result.IsEmpty() && result->IsString());
        v8::HandleScope handle_scope;
        v8::String::Utf8Value utf8(result);
        unsigned numchars = rtlUtf8Length(utf8.length(), *utf8);
        rtlUtf8ToUnicodeX(__chars, __result, numchars, *utf8);
    }
    virtual void getSetResult(bool & __isAllResult, size32_t & __resultBytes, void * & __result, int elemType, size32_t elemSize)
    {
        assertex (!result.IsEmpty());
        if (!result->IsArray())
            rtlFail(0, "v8embed: type mismatch - return value was not an array");
        v8::HandleScope handle_scope;
        v8::Handle<v8::Array> array = v8::Handle<v8::Array>::Cast(result);
        size_t numResults = array->Length();
        rtlRowBuilder out;
        byte *outData = NULL;
        size32_t outBytes = 0;
        if (elemSize != UNKNOWN_LENGTH)
        {
            out.ensureAvailable(numResults * elemSize); // MORE - check for overflow?
            outData = out.getbytes();
        }
        for (int i = 0; i < numResults; i++)
        {
            v8::Local<v8::Value> elem = array->Get(i);
            if (elem.IsEmpty())
                rtlFail(0, "v8embed: type mismatch - empty value in returned array");
            switch ((type_t) elemType)
            {
            case type_int:
                rtlWriteInt(outData, v8::Integer::Cast(*elem)->Value(), elemSize);
                break;
            case type_unsigned:
                rtlWriteInt(outData, v8::Integer::Cast(*elem)->Value(), elemSize);
                break;
            case type_real:
                if (elemSize == sizeof(double))
                    * (double *) outData = (double) v8::Number::Cast(*elem)->Value();
                else
                {
                    assertex(elemSize == sizeof(float));
                    * (float *) outData = (float) v8::Number::Cast(*elem)->Value();
                }
                break;
            case type_boolean:
                assertex(elemSize == sizeof(bool));
                * (bool *) outData = elem->BooleanValue();
                break;
            case type_string:
            case type_varstring:
            {
                if (!elem->IsString())
                    rtlFail(0, "v8embed: type mismatch - return value in list was not a STRING");
                v8::String::AsciiValue ascii(elem);
                const char * text =  *ascii;
                size_t lenBytes = ascii.length();
                if (elemSize == UNKNOWN_LENGTH)
                {
                    if (elemType == type_string)
                    {
                        out.ensureAvailable(outBytes + lenBytes + sizeof(size32_t));
                        outData = out.getbytes() + outBytes;
                        * (size32_t *) outData = lenBytes;
                        rtlStrToStr(lenBytes, outData+sizeof(size32_t), lenBytes, text);
                        outBytes += lenBytes + sizeof(size32_t);
                    }
                    else
                    {
                        out.ensureAvailable(outBytes + lenBytes + 1);
                        outData = out.getbytes() + outBytes;
                        rtlStrToVStr(0, outData, lenBytes, text);
                        outBytes += lenBytes + 1;
                    }
                }
                else
                {
                    if (elemType == type_string)
                        rtlStrToStr(elemSize, outData, lenBytes, text);
                    else
                        rtlStrToVStr(elemSize, outData, lenBytes, text);  // Fixed size null terminated strings... weird.
                }
                break;
            }
            case type_unicode:
            case type_utf8:
            {
                if (!elem->IsString())
                    rtlFail(0, "v8embed: type mismatch - return value in list was not a STRING");
                v8::String::Utf8Value utf8(elem);
                size_t lenBytes = utf8.length();
                const char * text =  *utf8;
                size32_t numchars = rtlUtf8Length(lenBytes, text);
                if (elemType == type_utf8)
                {
                    assertex (elemSize == UNKNOWN_LENGTH);
                    out.ensureAvailable(outBytes + lenBytes + sizeof(size32_t));
                    outData = out.getbytes() + outBytes;
                    * (size32_t *) outData = numchars;
                    rtlStrToStr(lenBytes, outData+sizeof(size32_t), lenBytes, text);
                    outBytes += lenBytes + sizeof(size32_t);
                }
                else
                {
                    if (elemSize == UNKNOWN_LENGTH)
                    {
                        out.ensureAvailable(outBytes + numchars*sizeof(UChar) + sizeof(size32_t));
                        outData = out.getbytes() + outBytes;
                        // You can't assume that number of chars in utf8 matches number in unicode16 ...
                        size32_t numchars16;
                        rtlDataAttr unicode16;
                        rtlUtf8ToUnicodeX(numchars16, unicode16.refustr(), numchars, text);
                        * (size32_t *) outData = numchars16;
                        rtlUnicodeToUnicode(numchars16, (UChar *) (outData+sizeof(size32_t)), numchars16, unicode16.getustr());
                        outBytes += numchars16*sizeof(UChar) + sizeof(size32_t);
                    }
                    else
                        rtlUtf8ToUnicode(elemSize / sizeof(UChar), (UChar *) outData, numchars, text);
                }
                break;
            }
            default:
                rtlFail(0, "v8embed: type mismatch - unsupported return type");
            }
            if (elemSize != UNKNOWN_LENGTH)
            {
                outData += elemSize;
                outBytes += elemSize;
            }
        }
        __isAllResult = false;
        __resultBytes = outBytes;
        __result = out.detachdata();
    }

    virtual void compileEmbeddedScript(size32_t lenChars, const char *utf)
    {
        v8::HandleScope handle_scope;
        v8::Handle<v8::String> source = v8::String::New(utf, rtlUtf8Size(lenChars, utf));
        v8::Handle<v8::Script> lscript = v8::Script::Compile(source);
        script = v8::Persistent<v8::Script>::New(lscript);
    }
    virtual void importFunction(size32_t lenChars, const char *utf)
    {
        UNIMPLEMENTED; // Not sure if meaningful for js
    }
    virtual void callFunction()
    {
        assertex (!script.IsEmpty());
        v8::HandleScope handle_scope;
        v8::TryCatch tryCatch;
        result = v8::Persistent<v8::Value>::New(script->Run());
        v8::Handle<v8::Value> exception = tryCatch.Exception();
        if (!exception.IsEmpty())
        {
            v8::String::AsciiValue msg(exception);
            throw MakeStringException(MSGAUD_user, 0, "v8embed: %s", *msg);
        }
    }

protected:
    v8::Isolate *isolate;
    v8::Persistent<v8::Context> context;
    v8::Persistent<v8::Script> script;
    v8::Persistent<v8::Value> result;
};

static __thread V8JavascriptEmbedFunctionContext * theFunctionContext;  // We reuse per thread, for speed
static __thread ThreadTermFunc threadHookChain;

static void releaseContext()
{
    if (theFunctionContext)
    {
        ::Release(theFunctionContext);
        theFunctionContext = NULL;
    }
    if (threadHookChain)
    {
        (*threadHookChain)();
        threadHookChain = NULL;
    }
}

class V8JavascriptEmbedContext : public CInterfaceOf<IEmbedContext>
{
public:
    V8JavascriptEmbedContext()
    {
    }
    virtual IEmbedFunctionContext *createFunctionContext(bool isImport, const char *options)
    {
        assertex(!isImport);
        if (!theFunctionContext)
        {
            theFunctionContext = new V8JavascriptEmbedFunctionContext;
            threadHookChain = addThreadTermFunc(releaseContext);
        }
        return LINK(theFunctionContext);
    }
} theEmbedContext;


extern IEmbedContext* getEmbedContext()
{
    return LINK(&theEmbedContext);
}

extern bool syntaxCheck(const char *script)
{
    return true; // MORE
}

} // namespace
