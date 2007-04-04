//
// Copyright (c) 2007 Ole Andr� Vadla Ravn�s <oleavr@gmail.com>
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//

#include "stdafx.h"
#include "Core.h"
#include "NullLogger.h"
#include "HookManager.h"
#include "Util.h"
#include <udis86.h>

#pragma warning( disable : 4311 4312 )

namespace InterceptPP {

static Logger *g_logger = NULL;

void
Initialize()
{
    Function::Initialize();
    Util::Instance()->Initialize();
    SetLogger(NULL);
}

void
UnInitialize()
{
    HookManager::Instance()->Reset();
    Logger *logger = static_cast<Logger *>(InterlockedExchangePointer(&g_logger, NULL));
    if (logger != NULL)
        delete logger;
    Util::Instance()->UnInitialize();
    Function::UnInitialize();
}

Logger *
GetLogger()
{
    return g_logger;
}

void
SetLogger(Logger *logger)
{
    if (logger == NULL)
        logger = new Logging::NullLogger();

    Logger *prevLogger = static_cast<Logger *>(InterlockedExchangePointer(&g_logger, logger));
    if (prevLogger != NULL)
        delete prevLogger;
}

const PrologSignatureSpec Function::prologSignatureSpecs[] = {
	{
		{
			NULL,
            0,
			"8B FF"					// mov edi, edi
			"55"					// push ebp
			"8B EC",				// mov ebp, esp
		},

		5,
	},
	{
		{
			NULL,
            0,
			"6A ??"					// push ??h
			"68 ?? ?? ?? ??"		// push offset dword_????????
			"E8 ?? ?? ?? ??",		// call __SEH_prolog
		},

		7,
	},
	{
		{
			NULL,
            0,
			"68 ?? ?? ?? ??"		// push ???h
			"68 ?? ?? ?? ??"		// push offset dword_????????
			"E8 ?? ?? ?? ??",		// call __SEH_prolog
		},

		5,
	},
	{
		{
			NULL,
            0,
			"FF 25 ?? ?? ?? ??"		// jmp ds:__imp__*
		},

		6,
	},
	{
		{
			NULL,
            0,
			"33 C0"		// xor eax, eax
			"50"		// push eax
			"50"		// push eax
			"6A ??"		// push ??
		},

		6,
	},
};

OVector<Signature>::Type Function::prologSignatures;

Logging::Node *
Argument::ToNode(bool deep, IPropertyProvider *propProv) const
{
    Logging::Element *el = new Logging::Element("Argument");
    el->AddField("Name", m_spec->GetName());

    Logging::Node *valueNode = m_spec->GetMarshaller()->ToNode(m_data, deep, propProv);
    if (valueNode != NULL)
        el->AppendChild(valueNode);

    return el;
}

OString
Argument::ToString(bool deep, IPropertyProvider *propProv) const
{
    return m_spec->GetMarshaller()->ToString(m_data, deep, propProv);
}

bool
Argument::ToInt(int &result) const
{
    return m_spec->GetMarshaller()->ToInt(m_data, result);
}

ArgumentListSpec::ArgumentListSpec()
{
    Initialize(0, NULL);
}

ArgumentListSpec::ArgumentListSpec(unsigned int count, ...)
{
    va_list args;
    va_start(args, count);

    Initialize(count, args);

    va_end(args);
}

ArgumentListSpec::ArgumentListSpec(unsigned int count, va_list args)
{
    Initialize(count, args);
}

ArgumentListSpec::~ArgumentListSpec()
{
    for (unsigned int i = 0; i < m_arguments.size(); i++)
    {
		delete m_arguments[i];
	}
}

void
ArgumentListSpec::Initialize(unsigned int count, va_list args)
{
    m_size = 0;
    m_hasOutArgs = false;

    for (unsigned int i = 0; i < count; i++)
    {
        ArgumentSpec *arg = va_arg(args, ArgumentSpec *);

        AddArgument(arg);
    }
}

void
ArgumentListSpec::AddArgument(ArgumentSpec *arg)
{
    m_arguments.push_back(arg);

    if ((arg->GetDirection() & ARG_DIR_OUT) != 0)
        m_hasOutArgs = true;

    m_size += arg->GetMarshaller()->GetSize();
}

ArgumentList::ArgumentList(ArgumentListSpec *spec, const void *data)
    : m_spec(spec)
{
    const void *p = data;

    for (unsigned int i = 0; i < spec->GetCount(); i++)
    {
        ArgumentSpec *argSpec = (*spec)[i];

        m_arguments.push_back(Argument(argSpec, p));

        p = static_cast<const unsigned char *>(p) + argSpec->GetSize();
    }
}

void
FunctionSpec::SetArguments(ArgumentListSpec *argList)
{
    if (argList != NULL)
    {
        m_argsSize = argList->GetSize();
        m_argList = argList;
    }
    else
    {
        m_argsSize = FUNCTION_ARGS_SIZE_UNKNOWN;
        m_argList = NULL;
    }
}

void
FunctionSpec::SetArguments(unsigned int count, ...)
{
    va_list args;
    va_start(args, count);

    m_argList = new ArgumentListSpec(count, args);
    m_argsSize = m_argList->GetSize();

    va_end(args);
}

Function::Function(FunctionSpec *spec, DWORD offset)
    : m_trampoline(NULL), m_oldMemProtect(0), m_origStart(0)
{
    Initialize(spec, offset);
}

void
Function::Initialize()
{
	for (int i = 0; i < sizeof(prologSignatureSpecs) / sizeof(PrologSignatureSpec); i++)
	{
        prologSignatures.push_back(Signature(prologSignatureSpecs[i].sig.signature));
    }
}

void
Function::UnInitialize()
{
    prologSignatures.clear();
}

OString
Function::GetFullName() const
{
    OOStringStream ss;

    const OString &parentName = GetParentName();
    if (parentName.length() > 0)
    {
        ss << parentName << "::";
    }

    ss << GetSpec()->GetName();

    return ss.str();
}

FunctionTrampoline *
Function::CreateTrampoline(unsigned int bytesToCopy)
{
    FunctionTrampoline *trampoline = reinterpret_cast<FunctionTrampoline *>(new unsigned char[sizeof(FunctionTrampoline) + bytesToCopy + sizeof(FunctionRedirectStub)]);

	trampoline->CALL_opcode = 0xE8;
	trampoline->CALL_offset = (DWORD) OnEnterProxy - (DWORD) &(trampoline->data);
	trampoline->data = this;

    if (bytesToCopy > 0)
    {
        memcpy(reinterpret_cast<unsigned char *>(trampoline) + sizeof(FunctionTrampoline), reinterpret_cast<const void *>(m_offset), bytesToCopy);
    }

    FunctionRedirectStub *redirStub = reinterpret_cast<FunctionRedirectStub *>(reinterpret_cast<unsigned char *>(trampoline) + sizeof(FunctionTrampoline) + bytesToCopy);
    redirStub->JMP_opcode = 0xE9;
    redirStub->JMP_offset = (m_offset + bytesToCopy) - (reinterpret_cast<DWORD>(reinterpret_cast<unsigned char *>(redirStub) + sizeof(FunctionRedirectStub)));

	return trampoline;
}

void
Function::Hook()
{
    const PrologSignatureSpec *spec = NULL;
    int nBytesToCopy = 0;

    for (unsigned int i = 0; i < prologSignatures.size(); i++)
    {
        const Signature *sig = &prologSignatures[i];

        OVector<void *>::Type matches = SignatureMatcher::Instance()->FindInRange(*sig, reinterpret_cast<void *>(m_offset), sig->GetLength());
        if (matches.size() == 1)
        {
            spec = &prologSignatureSpecs[i];
            break;
        }
    }

    if (spec != NULL)
    {
        nBytesToCopy = spec->numBytesToCopy;
    }
    else
    {
        unsigned char *p = reinterpret_cast<unsigned char *>(m_offset);
        const int bytesNeeded = 5;

        ud_t udObj;
        ud_init(&udObj);
        ud_set_input_buffer(&udObj, p, 16);

        while (nBytesToCopy < bytesNeeded)
        {
            int size = ud_disassemble(&udObj);
            if (size == 0)
                throw Error("none of the supported signatures matched and libudis86 fallback failed as well");

            nBytesToCopy += size;
        }

        GetLogger()->LogDebug("Calculated that we need to copy %d bytes of the original function", nBytesToCopy);
    }

    FunctionTrampoline *trampoline = CreateTrampoline(nBytesToCopy);
    m_trampoline = trampoline;

    if (!VirtualProtect(reinterpret_cast<LPVOID>(m_offset), sizeof(LONGLONG), PAGE_EXECUTE_WRITECOPY, &m_oldMemProtect))
        throw Error("VirtualProtected failed");

    FunctionRedirectStub *redirStub = reinterpret_cast<FunctionRedirectStub *>(m_offset);

    LONGLONG buf;
    memcpy(&buf, redirStub, sizeof(buf));
    m_origStart = buf;

    FunctionRedirectStub *stub = reinterpret_cast<FunctionRedirectStub *>(&buf);
    stub->JMP_opcode = 0xE9;
    stub->JMP_offset = reinterpret_cast<DWORD>(trampoline) - (reinterpret_cast<DWORD>(reinterpret_cast<unsigned char *>(redirStub) + sizeof(FunctionRedirectStub)));

    LONGLONG *dst = reinterpret_cast<LONGLONG *>(redirStub);
#if 1
    *dst = buf;
#else
    // Only available on win2k3 and newer *sigh*
    InterlockedExchange64(dst, buf);
#endif

    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
}

void
Function::UnHook()
{
    LONGLONG *dst = reinterpret_cast<LONGLONG *>(m_offset);

#if 1
    *dst = m_origStart;
#else
    InterlockedExchange64(dst, m_origStart);
#endif

    FlushInstructionCache(GetCurrentProcess(), NULL, 0);

    delete m_trampoline;
    m_trampoline = NULL;
}

__declspec(naked) void
Function::OnEnterProxy(CpuContext cpuCtx, unsigned int unwindSize, FunctionTrampoline *trampoline, void **proxyRet, void **finalRet)
{
	DWORD lastError;
	Function *function;
	FunctionTrampoline *nextTrampoline;

	__asm {
											// *** We're coming in hot from the modified vtable through the trampoline ***

		sub esp, 12;						//  1. Reserve space for the last 3 arguments.

		push 16;							//  2. Set unwindSize to the size of the last 4 arguments.

		pushad;								//  3. Save all registers and conveniently place them so that they're available
											//     from C++ through the first argument.

		lea eax, [esp+48+4];				//  4. Set finalRet to point to the final return address.
		mov [esp+48-4], eax;

		lea eax, [esp+48+0];				//  5. Set proxyRet to point to this function's return address.
		mov [esp+48-8], eax;

		mov eax, [eax];						//  6. Set trampoline to point to the start of the trampoline, ie. *proxyRet - 5.
        sub eax, 5;
		mov [esp+48-12], eax;

		sub esp, 4;							//  7. Padding/fake return address so that ebp+8 refers to the first argument.
		push ebp;							//  8. Standard prolog.
		mov ebp, esp;
		sub esp, __LOCAL_SIZE;
	}

	lastError = GetLastError();

	function = static_cast<Function *>(trampoline->data);
	nextTrampoline = function->OnEnterWrapper(&cpuCtx, &unwindSize, trampoline, finalRet, &lastError);
	if (nextTrampoline != NULL)
	{
		*proxyRet = reinterpret_cast<unsigned char *>(trampoline) + sizeof(FunctionTrampoline);
		*finalRet = nextTrampoline;
	}

	SetLastError(lastError);

	__asm {
											// *** Bounce off to the actual method, or straight back to the caller. ***

		mov esp, ebp;						//  1. Standard epilog.
		pop ebp;
		add esp, 4;							//  2. Remove the padding/fake return address (see step 7 above).

		popad;								//  3. Clean up the first argument and restore the registers (see step 3 above).

		add esp, [esp];						//  4. Clean up the remaining arguments (and more if returning straight back).

		ret;
	}
}

FunctionTrampoline *
Function::OnEnterWrapper(CpuContext *cpuCtx, unsigned int *unwindSize, FunctionTrampoline *trampoline, void *btAddr, DWORD *lastError)
{
	// Keep track of the function call
	FunctionCall *call = new FunctionCall(this, btAddr, cpuCtx);
	call->SetCpuContextLive(cpuCtx);
	call->SetLastErrorLive(lastError);

	OnEnter(call);

	bool carryOn = call->GetShouldCarryOn();

	FunctionSpec *spec = call->GetFunction()->GetSpec();
	CallingConvention conv = spec->GetCallingConvention();
	if (conv == CALLING_CONV_UNKNOWN ||
		(conv != CALLING_CONV_CDECL && spec->GetArgsSize() == FUNCTION_ARGS_SIZE_UNKNOWN))
	{
		// TODO: log a warning here
		carryOn = true;
	}

	if (carryOn)
	{
		// Set up a trampoline used to trap the return
		FunctionTrampoline *retTrampoline = new FunctionTrampoline;

		retTrampoline->CALL_opcode = 0xE8;
		retTrampoline->CALL_offset = (DWORD) Function::OnLeaveProxy - (DWORD) &(retTrampoline->data);
		retTrampoline->data = call;

		return retTrampoline;
	}
	else
	{	
		// Clear off the proxy return address.
		*unwindSize += sizeof(void *);

		if (conv != CALLING_CONV_CDECL)
		{
			*unwindSize += spec->GetArgsSize();

			void **retAddr = reinterpret_cast<void **>(static_cast<char *>(btAddr) + spec->GetArgsSize());
			*retAddr = call->GetReturnAddress();
		}
	}

	delete call;
	return NULL;
}

__declspec(naked) void
Function::OnLeaveProxy(CpuContext cpuCtx, FunctionTrampoline *trampoline)
{
	FunctionCall *call;
	DWORD lastError;

	__asm {
											// *** We're coming in hot and the method has just been called ***

		sub esp, 4;							//  1. Reserve space for the second argument to this function (VMethodTrampoline *).
		push eax;
		push ebx;
		mov eax, [esp+8+4];					//  2. Get the trampoline returnaddress, which is the address of the VMethodCall *
											//     right after the CALL instruction on the trampoline.
		mov ebx, eax;						//  3. Store the VMethodCall ** in ebx.
		mov ebx, [ebx];						//  4. Dereference the VMethodCall **.
		sub eax, 5;							//  5. Rewind the pointer to the start of the VMethodTrampoline structure.
		mov [esp+8+0], eax;					//  6. Store the VMethodTrampoline * on the reserved spot so that we can access it from
											//     C++ through the second argument.
		mov eax, [ebx+FunctionCall::m_returnAddress];	//  6. Get the return address of the caller.
		mov [esp+8+4], eax;					//  7. Replace the trampoline return-address with the return address of the caller.
		pop ebx;
		pop eax;

		pushad;								//  8. Save all registers and conveniently place them so that they're available
											//     from C++ through the first argument.

		sub esp, 4;							//  9. Padding/fake return address so that ebp+8 refers to the first argument.
		push ebp;							// 10. Standard prolog.
		mov ebp, esp;
		sub esp, __LOCAL_SIZE;
	}

    lastError = GetLastError();

	call = static_cast<FunctionCall *>(trampoline->data);
	call->GetFunction()->OnLeaveWrapper(&cpuCtx, trampoline, call, &lastError);

    SetLastError(lastError);

	__asm {
											// *** Bounce off back to the caller ***

		mov esp, ebp;						//  1. Standard epilog.
		pop ebp;
		add esp, 4;							//  2. Remove the padding/fake return address (see step 10 above).

		popad;								//  3. Clean up the first argument and restore the registers (see step 9 above).

		add esp, 4;							//  4. Clean up the second argument.
		ret;								//  5. Bounce to the caller.
	}
}

void
Function::OnLeaveWrapper(CpuContext *cpuCtx, FunctionTrampoline *trampoline, FunctionCall *call, DWORD *lastError)
{
    call->SetState(FUNCTION_CALL_LEAVING);

	call->SetCpuContextLive(cpuCtx);
	call->SetLastErrorLive(lastError);

    // Got this now
	call->SetCpuContextLeave(cpuCtx);

	// Do some logging
	OnLeave(call);

	delete trampoline;
	delete call;
}

void
Function::OnEnter(FunctionCall *call)
{
	FunctionCallHandler handler = call->GetFunction()->GetSpec()->GetHandler();

	if (handler == NULL || !handler(call))
	{        
        Logging::Event *ev = GetLogger()->NewEvent("FunctionCall");

        Logging::TextNode *textNode = new Logging::TextNode("Name", GetFullName());
        ev->AppendChild(textNode);

        call->AppendBacktraceToElement(ev);
        call->AppendCpuContextToElement(ev);
        call->AppendArgumentsToElement(ev);

        call->SetUserData(ev);
	}
}

void
Function::OnLeave(FunctionCall *call)
{
	FunctionCallHandler handler = call->GetFunction()->GetSpec()->GetHandler();

	if (handler == NULL || !handler(call))
	{
        Logging::Event *ev = static_cast<Logging::Event *>(call->GetUserData());

        call->AppendCpuContextToElement(ev);
        call->AppendArgumentsToElement(ev);

        ev->Submit();
	}
}

FunctionCall::FunctionCall(Function *function, void *btAddr, CpuContext *cpuCtxEnter)
    : m_function(function), m_backtraceAddress(btAddr),
      m_returnAddress(*((void **) btAddr)),
      m_cpuCtxLive(NULL), m_cpuCtxEnter(*cpuCtxEnter),
      m_lastErrorLive(NULL),
      m_arguments(NULL),
      m_state(FUNCTION_CALL_ENTERING),
      m_shouldCarryOn(true),
      m_userData(NULL)
{
	memset(&m_cpuCtxLeave, 0, sizeof(m_cpuCtxLeave));

	int argsSize = function->GetSpec()->GetArgsSize();
	if (argsSize != FUNCTION_ARGS_SIZE_UNKNOWN)
	{
		m_argumentsData.resize(argsSize);
		memcpy((void *) m_argumentsData.data(), (BYTE *) btAddr + 4, argsSize);

        ArgumentListSpec *spec = function->GetSpec()->GetArguments();
        if (spec != NULL)
        {
            m_arguments = new ArgumentList(spec, m_argumentsData.data());
        }
	}
}

bool
FunctionCall::ShouldLogArgumentDeep(const Argument *arg) const
{
    ArgumentDirection dir = arg->GetSpec()->GetDirection();

    if (m_state == FUNCTION_CALL_ENTERING)
    {
        return (dir & ARG_DIR_IN) != 0;
    }
    else
    {
        return (dir & ARG_DIR_OUT) != 0;
    }
}

void
FunctionCall::AppendBacktraceToElement(Logging::Element *el)
{
    Logging::Node *btNode = Util::Instance()->CreateBacktraceNode(m_backtraceAddress);
    if (btNode != NULL)
    {
        el->AppendChild(btNode);
    }
}

void
FunctionCall::AppendCpuContextToElement(Logging::Element *el)
{
    Logging::Element *ctxEl = new Logging::Element("CpuContext");

    ctxEl->AddField("Direction", (m_state == FUNCTION_CALL_ENTERING) ? "In" : "Out");

    AppendCpuRegisterToElement(ctxEl, "eax", m_cpuCtxLive->eax);
    AppendCpuRegisterToElement(ctxEl, "ebx", m_cpuCtxLive->ebx);
    AppendCpuRegisterToElement(ctxEl, "ecx", m_cpuCtxLive->ecx);
    AppendCpuRegisterToElement(ctxEl, "edx", m_cpuCtxLive->edx);
    AppendCpuRegisterToElement(ctxEl, "edi", m_cpuCtxLive->edi);
    AppendCpuRegisterToElement(ctxEl, "esi", m_cpuCtxLive->esi);
    AppendCpuRegisterToElement(ctxEl, "ebp", m_cpuCtxLive->ebp);
    AppendCpuRegisterToElement(ctxEl, "esp", m_cpuCtxLive->esp);

    el->AppendChild(ctxEl);
}

void
FunctionCall::AppendCpuRegisterToElement(Logging::Element *el, const char *name, DWORD value)
{
    Logging::Element *regEl = new Logging::Element("Register");
    el->AppendChild(regEl);
    regEl->AddField("Name", name);

    OOStringStream ss;
    ss << "0x" << hex << value;
    regEl->AddField("Value", ss.str());
}

void
FunctionCall::AppendArgumentsToElement(Logging::Element *el)
{
	FunctionSpec *spec = m_function->GetSpec();

    const ArgumentList *args = GetArguments();
    if (args != NULL)
    {
        bool logIt = true;

        // Do we have any out arguments?
        if (m_state == FUNCTION_CALL_LEAVING && !args->GetSpec()->GetHasOutArgs())
        {
            logIt = false;
        }

        if (logIt)
        {
            Logging::Element *argsEl = new Logging::Element("Arguments");
            el->AppendChild(argsEl);
            argsEl->AddField("Direction", (m_state == FUNCTION_CALL_ENTERING) ? "In" : "Out");

            for (unsigned int i = 0; i < args->GetCount(); i++)
            {
                const Argument &arg = (*args)[i];

                if (!(m_state == FUNCTION_CALL_LEAVING && arg.GetSpec()->GetDirection() == ARG_DIR_IN))
			        argsEl->AppendChild(arg.ToNode(ShouldLogArgumentDeep(&arg), this));
            }
        }
    }
    else
    {
        // No point in logging for this state in this case
        if (m_state == FUNCTION_CALL_LEAVING)
            return;

        Logging::Element *argsEl = new Logging::Element("Arguments");
        el->AppendChild(argsEl);
        argsEl->AddField("Direction", "In");

	    int argsSize = spec->GetArgsSize();
	    if (argsSize != FUNCTION_ARGS_SIZE_UNKNOWN && argsSize % sizeof(DWORD) == 0)
	    {
		    DWORD *args = (DWORD *) m_argumentsData.data();

			Marshaller::UInt32 marshaller;

		    for (unsigned int i = 0; i < argsSize / sizeof(DWORD); i++)
		    {
				Logging::Element *argElement = new Logging::Element("Argument");
                argsEl->AppendChild(argElement);

				OOStringStream ss;
				ss << "Arg" << (i + 1);
				argElement->AddField("Name", ss.str());

				bool hex = false;

				// FIXME: optimize this
				if (args[i] > 0xFFFF && !IsBadReadPtr((void *) args[i], 1))
					hex = true;

				marshaller.SetFormatHex(hex);

                Logging::Node *valueNode = marshaller.ToNode(&args[i], true, this);
                if (valueNode != NULL)
                    argElement->AppendChild(valueNode);
		    }
	    }
    }
}

OString
FunctionCall::ToString()
{
	FunctionSpec *spec = m_function->GetSpec();

	OOStringStream ss;

	ss << m_function->GetFullName();

    const ArgumentList *args = GetArguments();
    if (args != NULL)
    {
        ss << "(";

        for (unsigned int i = 0; i < args->GetCount(); i++)
        {
            const Argument &arg = (*args)[i];

            if (i)
			    ss << ", ";

            ss << arg.ToString(ShouldLogArgumentDeep(&arg), this);
        }

        ss << ")";
    }
    else
    {
	    int argsSize = spec->GetArgsSize();
	    if (argsSize != FUNCTION_ARGS_SIZE_UNKNOWN && argsSize % sizeof(DWORD) == 0)
	    {
		    ss << "(";

		    DWORD *args = (DWORD *) m_argumentsData.data();

		    for (unsigned int i = 0; i < argsSize / sizeof(DWORD); i++)
		    {
			    if (i)
				    ss << ", ";

			    // FIXME: optimize this
			    if (args[i] > 0xFFFF && !IsBadReadPtr((void *) args[i], 1))
				    ss << hex << "0x";
			    else
				    ss << dec;

			    ss << args[i];
		    }

		    ss << ")";
	    }
    }

	return ss.str();
}

bool
FunctionCall::QueryForProperty(const OString &query, int &result)
{
    OString propObj = query.substr(0, 4);
    OString propArg = query.substr(4);

    if (propObj == "reg.")
    {
        if (propArg == "eax")
        {
            result = m_cpuCtxLive->eax;
            return true;
        }
        // TODO: complete this and make it somewhat more elegant
    }
    else if (propObj == "arg.")
    {
        for (unsigned int i = 0; i < m_arguments->GetCount(); i++)
        {
            const Argument &arg = (*m_arguments)[i];

            if (arg.GetSpec()->GetName() == propArg)
            {
                return arg.ToInt(result);
            }
        }
    }

	return false;
}

} // namespace InterceptPP