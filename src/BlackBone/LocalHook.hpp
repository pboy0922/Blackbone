#pragma once

#include "Config.h"
#include "HookHandlers.h"
#include "Process.h"

namespace blackbone
{

template<typename Fn, class C = NoClass>
class Detour : public HookHandler<Fn, C>
{
public:
    typedef typename HookHandler<Fn, C>::type type;
    typedef typename HookHandler<Fn, C>::hktype hktype;
    typedef typename HookHandler<Fn, C>::hktypeC hktypeC;

public:  
    Detour()
    {
        this->_internalHandler = &HookHandler<Fn, C>::Handler;
    }

    ~Detour()
    {
        Restore();
    }

    /// <summary>
    /// Hook function
    /// </summary>
    /// <param name="ptr">Target function address</param>
    /// <param name="hkPtr">Hook function address</param>
    /// <param name="type">Hooking method</param>
    /// <param name="order">Call order. Hook before original or vice versa</param>
    /// <param name="retType">Return value. Use origianl or value from hook</param>
    /// <returns>true on success</returns>
    bool Hook( type ptr, hktype hkPtr, HookType::e type,
               CallOrder::e order = CallOrder::HookFirst,
               ReturnMethod::e retType = ReturnMethod::UseOriginal )
    { 
        if (this->_hooked)
            return false;

        this->_type  = type;
        this->_order = order;
        this->_retType = retType;
        this->_callOriginal = this->_original = ptr;
        this->_callback = hkPtr;

        switch (this->_type)
        {
            case HookType::Inline:
                return HookInline();

            case HookType::Int3:
                return HookInt3();

            case HookType::HWBP:
                return HookHWBP();

            default:
                return false;
        }
    }

    /// <summary>
    /// Hook function
    /// </summary>
    /// <param name="Ptr">Target function address</param>
    /// <param name="hkPtr">Hook class member address</param>
    /// <param name="pClass">Hook class address</param>
    /// <param name="type">Hooking method</param>
    /// <param name="order">Call order. Hook before original or vice versa</param>
    /// <param name="retType">Return value. Use origianl or value from hook</param>
    /// <returns>true on success</returns>
    bool Hook( type Ptr, hktypeC hkPtr, C* pClass, HookType::e type,
               CallOrder::e order = CallOrder::HookFirst,
               ReturnMethod::e retType = ReturnMethod::UseOriginal )
    {
        this->_callbackClass = pClass;
        return Hook( Ptr, brutal_cast<hktype>(hkPtr), type, order, retType );
    }


    /// <summary>
    /// Restore hooked function
    /// </summary>
    /// <returns>true on success, false if not hooked</returns>
    bool Restore()
    {
        if (!this->_hooked)
            return false;
        
        switch (this->_type)
        {
            case HookType::Inline:
            case HookType::InternalInline:
            case HookType::Int3:
                WriteProcessMemory( GetCurrentProcess(), this->_original, this->_origCode, this->_origSize, NULL );
                break;

            case HookType::HWBP:
                {
                    Process thisProc;
                    thisProc.Attach( GetCurrentProcessId() );

                    for (auto& thd : thisProc.threads().getAll())
                        thd.RemoveHWBP( reinterpret_cast<ptr_t>(this->_original) );

                    this->_hwbpIdx.clear();
                }
                break;

            default:
                break;
        }

        this->_hooked = false;
        return true;
    }

private:

    /// <summary>
    /// Perform inline hook
    /// </summary>
    /// <returns>true on success</returns>
    bool HookInline()
    {
        AsmJit::Assembler jmpToHook, jmpToThunk; 

        //
        // Construct jump to thunk
        //
#ifdef USE64
        jmpToThunk.mov( AsmJit::rax, (uint64_t)this->_buf );
        jmpToThunk.jmp( AsmJit::rax );

        this->_origSize = jmpToThunk.getCodeSize( );
#else
        jmpToThunk.jmp( _buf );
        this->_origSize = jmpToThunk.getCodeSize();
#endif
        
        DetourBase::CopyOldCode( (uint8_t*)this->_original );

        // Construct jump to hook handler
#ifdef USE64
        // mov gs:[0x28], this
        jmpToHook.mov( AsmJit::rax, (uint64_t)this );
        jmpToHook.mov( AsmJit::qword_ptr_abs( (void*)0x28, 0, AsmJit::SEGMENT_GS ), AsmJit::rax );
#else
        // mov fs:[0x14], this
        jmpToHook.mov( AsmJit::dword_ptr_abs( (void*)0x14, 0, AsmJit::SEGMENT_FS ), (uint32_t)this );
#endif // USE64

        jmpToHook.jmp( &HookHandler<Fn, C>::Handler );
        jmpToHook.relocCode( this->_buf );

        BOOL res = WriteProcessMemory( GetCurrentProcess(), this->_original, this->_newCode,
                                       jmpToThunk.relocCode( this->_newCode, (sysuint_t)this->_original ), NULL );
        
        return (this->_hooked = (res == TRUE));
    }

    /// <summary>
    /// Perform int3 hook
    /// </summary>
    /// <returns>true on success</returns>
    bool HookInt3()
    {
        this->_newCode[0] = 0xCC;
        this->_origSize = sizeof( this->_newCode[0] );

        // Setup handler
        if (this->_vecHandler == nullptr)
            this->_vecHandler = AddVectoredExceptionHandler( 1, &DetourBase::VectoredHandler );

        if (!this->_vecHandler)
            return false;

        this->_breakpoints.insert( std::make_pair( this->_original, (DetourBase*)this ) );

        // Save original code
        memcpy( this->_origCode, this->_original, this->_origSize );

        // Write break instruction
        BOOL res = WriteProcessMemory( GetCurrentProcess(), this->_original, this->_newCode, this->_origSize, NULL );

        return (this->_hooked = (res == TRUE));
    }

    /// <summary>
    /// Perform hardware breakpoint hook
    /// </summary>
    /// <returns>true on success</returns>
    bool HookHWBP()
    {
        Process thisProc;
        thisProc.Attach( GetCurrentProcessId() );

        // Setup handler
        if (this->_vecHandler == nullptr)
            this->_vecHandler = AddVectoredExceptionHandler( 1, &DetourBase::VectoredHandler );

        if (!this->_vecHandler)
            return false;

        this->_breakpoints.insert( std::make_pair( this->_original, (DetourBase*)this ) );

        // Add breakpoint to every thread
        for (auto& thd : thisProc.threads().getAll())
            this->_hwbpIdx[thd.id()] = thd.AddHWBP( reinterpret_cast<ptr_t>(this->_original), hwbp_execute, hwbp_1 );
    
        return this->_hooked = true;
    }
};

}