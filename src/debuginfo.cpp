// --- storing and accessing source location metadata ---

struct FuncInfo{
    const Function* func;
    size_t lengthAdr;
    std::string name;
    std::string filename;
#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)
    PRUNTIME_FUNCTION fnentry;
#endif
    std::vector<JITEvent_EmittedFunctionDetails::LineStart> lines;
};

#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)
#include <dbghelp.h>
extern "C" EXCEPTION_DISPOSITION _seh_exception_handler(PEXCEPTION_RECORD ExceptionRecord,void *EstablisherFrame, PCONTEXT ContextRecord, void *DispatcherContext);
extern "C" volatile int jl_in_stackwalk;
#endif

struct revcomp {
  bool operator() (const size_t& lhs, const size_t& rhs) const
  {return lhs>rhs;}
};

class JuliaJITEventListener: public JITEventListener
{
    std::map<size_t, FuncInfo, revcomp> info;

public:
    JuliaJITEventListener(){}
    virtual ~JuliaJITEventListener() {}

    virtual void NotifyFunctionEmitted(const Function &F, void *Code,
                                       size_t Size, const EmittedFunctionDetails &Details)
    {
#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)
        assert(!jl_in_stackwalk);
        jl_in_stackwalk = 1;
        uintptr_t catchjmp = (uintptr_t)Code+Size;
        *(uint8_t*)(catchjmp+0) = 0x48;
        *(uint8_t*)(catchjmp+1) = 0xb8; // mov RAX, QWORD PTR [...]
        *(uint64_t*)(catchjmp+2) = (uint64_t)&_seh_exception_handler;
        *(uint8_t*)(catchjmp+10) = 0xff;
        *(uint8_t*)(catchjmp+11) = 0xe0; // jmp RAX
        PRUNTIME_FUNCTION tbl = (PRUNTIME_FUNCTION)((catchjmp+12+3)&~(uintptr_t)3);
        uint8_t *UnwindData = (uint8_t*)((((uintptr_t)&tbl[1])+3)&~(uintptr_t)3);
        RUNTIME_FUNCTION fn = {0,(DWORD)Size+13,(DWORD)(intptr_t)(UnwindData-(uint8_t*)Code)};
        tbl[0] = fn;
        UnwindData[0] = 0x09; // version info, UNW_FLAG_EHANDLER
        UnwindData[1] = 4;    // size of prolog (bytes)
        UnwindData[2] = 2;    // count of unwind codes (slots)
        UnwindData[3] = 0x05; // frame register (rbp) = rsp
        UnwindData[4] = 4;    // second instruction
        UnwindData[5] = 0x03; // mov RBP, RSP
        UnwindData[6] = 1;    // first instruction
        UnwindData[7] = 0x50; // push RBP
        *(DWORD*)&UnwindData[8] = (DWORD)(catchjmp-(intptr_t)Code);
        DWORD mod_size = (DWORD)(size_t)(&UnwindData[8]-(uint8_t*)Code);
        if (!SymLoadModuleEx(GetCurrentProcess(), NULL, NULL, NULL, (DWORD64)Code, mod_size, NULL, SLMFLAG_VIRTUAL)) {
            JL_PRINTF(JL_STDERR, "WARNING: failed to insert function info for backtrace\n");
        }
        else {
            if (!SymAddSymbol(GetCurrentProcess(), (ULONG64)Code, F.getName().data(), (DWORD64)Code, mod_size, 0)) {
                JL_PRINTF(JL_STDERR, "WARNING: failed to insert function name into debug info\n");
            }
            if (!RtlAddFunctionTable(tbl,1,(DWORD64)Code)) {
                JL_PRINTF(JL_STDERR, "WARNING: failed to insert function stack unwind info\n");
            }
        }
        jl_in_stackwalk = 0;

        FuncInfo tmp = {&F, Size, std::string(), std::string(), tbl, Details.LineStarts};
#else
        FuncInfo tmp = {&F, Size, std::string(), std::string(), Details.LineStarts};
#endif
        if (tmp.lines.size() != 0) info[(size_t)(Code)] = tmp;
    }

    std::map<size_t, FuncInfo, revcomp>& getMap()
    {
        return info;
    }
};

JuliaJITEventListener *jl_jit_events;

extern "C" void jl_getFunctionInfo(const char **name, int *line, const char **filename,size_t pointer);

void jl_getFunctionInfo(const char **name, int *line, const char **filename, size_t pointer)
{
    std::map<size_t, FuncInfo, revcomp> &info = jl_jit_events->getMap();
    *name = NULL;
    *line = -1;
    *filename = "no file";
    std::map<size_t, FuncInfo, revcomp>::iterator it = info.lower_bound(pointer);
    if (it != info.end() && (size_t)(*it).first + (*it).second.lengthAdr >= pointer) {
        // commenting these lines out skips functions that don't
        // have explicit debug info. this is useful for hiding
        // the jlcall wrapper functions we generate.
#if LLVM_VERSION_MAJOR == 3
#if LLVM_VERSION_MINOR == 0
        //*name = &(*(*it).second.func).getNameStr()[0];
#elif LLVM_VERSION_MINOR >= 1
        //*name = (((*(*it).second.func).getName()).data());
#endif
#endif
        std::vector<JITEvent_EmittedFunctionDetails::LineStart>::iterator vit = (*it).second.lines.begin();
        JITEvent_EmittedFunctionDetails::LineStart prev = *vit;

        if ((*it).second.func) {
            DISubprogram debugscope =
                DISubprogram(prev.Loc.getScope((*it).second.func->getContext()));
            *filename = debugscope.getFilename().data();
            // the DISubprogram has the un-mangled name, so use that if
            // available.
            *name = debugscope.getName().data();
        } else {
            *name = (*it).second.name.c_str();
            *filename = (*it).second.filename.c_str();
        }

        vit++;

        while (vit != (*it).second.lines.end()) {
            if (pointer <= (*vit).Address) {
                *line = prev.Loc.getLine();
                break;
            }
            prev = *vit;
            vit++;
        }
        if (*line == -1) {
            *line = prev.Loc.getLine();
        }
    }
}

#if defined(_OS_WINDOWS_) && defined(_CPU_X86_64_)

extern "C" void* CALLBACK jl_getUnwindInfo(HANDLE hProcess, ULONG64 AddrBase, ULONG64 UserContext);

void* CALLBACK jl_getUnwindInfo(HANDLE hProcess, ULONG64 AddrBase, ULONG64 UserContext)
{
    std::map<size_t, FuncInfo, revcomp> &info = jl_jit_events->getMap();
    std::map<size_t, FuncInfo, revcomp>::iterator it = info.lower_bound(AddrBase);
    if (it != info.end() && (size_t)(*it).first + (*it).second.lengthAdr >= AddrBase) {
        return (void*)(*it).second.fnentry;
    }
    return NULL;
}

// Custom memory manager for exception handling on Windows
// we overallocate 48 bytes at the end of each function
// for unwind information (see NotifyFunctionEmitted)
class JITMemoryManagerWin : public JITMemoryManager {
private:
  JITMemoryManager *JMM;
public:
  JITMemoryManagerWin() : JITMemoryManager() {
      JMM = JITMemoryManager::CreateDefaultMemManager();
  }
  virtual void setMemoryWritable() { return JMM->setMemoryWritable(); }
  virtual void setMemoryExecutable() { return JMM->setMemoryExecutable(); }
  virtual void setPoisonMemory(bool poison) { return JMM->setPoisonMemory(poison); }
  virtual void AllocateGOT() { JMM->AllocateGOT(); HasGOT = true; }
  virtual uint8_t *getGOTBase() const { return JMM->getGOTBase(); }
  virtual uint8_t *startFunctionBody(const Function *F,
                                     uintptr_t &ActualSize) { ActualSize += 48; uint8_t *ret = JMM->startFunctionBody(F,ActualSize); ActualSize -= 48; return ret; }
  virtual uint8_t *allocateStub(const GlobalValue* F, unsigned StubSize,
                                unsigned Alignment)  { return JMM->allocateStub(F,StubSize,Alignment); }
  virtual void endFunctionBody(const Function *F, uint8_t *FunctionStart,
                               uint8_t *FunctionEnd) { return JMM->endFunctionBody(F,FunctionStart,FunctionEnd+48); }
  virtual uint8_t *allocateSpace(intptr_t Size, unsigned Alignment) { return JMM->allocateSpace(Size,Alignment); }
  virtual uint8_t *allocateGlobal(uintptr_t Size, unsigned Alignment) { return JMM->allocateGlobal(Size,Alignment); }
  virtual void deallocateFunctionBody(void *Body) { return JMM->deallocateFunctionBody(Body); }
  virtual uint8_t* startExceptionTable(const Function* F,
                                       uintptr_t &ActualSize) { return JMM->startExceptionTable(F,ActualSize); }
  virtual void endExceptionTable(const Function *F, uint8_t *TableStart,
                                 uint8_t *TableEnd, uint8_t* FrameRegister) { return JMM->endExceptionTable(F,TableStart,TableEnd,FrameRegister); }
  virtual void deallocateExceptionTable(void *ET) { return JMM->deallocateExceptionTable(ET); }
  virtual bool CheckInvariants(std::string &str) { return JMM->CheckInvariants(str); }
  virtual size_t GetDefaultCodeSlabSize() { return JMM->GetDefaultCodeSlabSize(); }
  virtual size_t GetDefaultDataSlabSize() { return JMM->GetDefaultDataSlabSize(); }
  virtual size_t GetDefaultStubSlabSize() { return JMM->GetDefaultStubSlabSize(); }
  virtual unsigned GetNumCodeSlabs() { return JMM->GetNumCodeSlabs(); }
  virtual unsigned GetNumDataSlabs() { return JMM->GetNumDataSlabs(); }
  virtual unsigned GetNumStubSlabs() { return JMM->GetNumStubSlabs(); }

  virtual uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID) { return JMM->allocateCodeSection(Size,Alignment,SectionID); }
  virtual uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                       unsigned SectionID, bool IsReadOnly) { return JMM->allocateDataSection(Size,Alignment,SectionID,IsReadOnly); }
  virtual void *getPointerToNamedFunction(const std::string &Name,
                                          bool AbortOnFailure = true) { return JMM->getPointerToNamedFunction(Name,AbortOnFailure); }
  virtual bool applyPermissions(std::string *ErrMsg = 0) { return JMM->applyPermissions(ErrMsg); }
  virtual void registerEHFrames(StringRef SectionData) { return JMM->registerEHFrames(SectionData); }
};
#endif

// Code coverage

typedef std::map<std::string,std::vector<GlobalVariable*> > coveragedata_t;
static coveragedata_t coverageData;

static void coverageVisitLine(std::string filename, int line)
{
    if (filename == "" || filename == "none" || filename == "no file")
        return;
    coveragedata_t::iterator it = coverageData.find(filename);
    if (it == coverageData.end()) {
        coverageData[filename] = std::vector<GlobalVariable*>(0);
    }
    std::vector<GlobalVariable*> &vec = coverageData[filename];
    if (vec.size() <= (size_t)line)
        vec.resize(line+1, NULL);
    if (vec[line] == NULL)
        vec[line] = new GlobalVariable(*jl_Module, T_int64, false, GlobalVariable::InternalLinkage,
                                       ConstantInt::get(T_int64,0), "lcnt");
    GlobalVariable *v = vec[line];
    builder.CreateStore(builder.CreateAdd(builder.CreateLoad(v),
                                          ConstantInt::get(T_int64,1)),
                        v);
}

extern "C" void jl_write_coverage_data(void)
{
    coveragedata_t::iterator it = coverageData.begin();
    for (; it != coverageData.end(); it++) {
        std::string filename = (*it).first;
        std::string outfile = filename + ".cov";
        std::vector<GlobalVariable*> &counts = (*it).second;
        if (counts.size() > 1) {
            std::ifstream inf(filename.c_str());
            if (inf.is_open()) {
                std::ofstream outf(outfile.c_str(), std::ofstream::trunc | std::ofstream::out);
                char line[1024];
                int l = 1;
                while (!inf.eof()) {
                    inf.getline(line, sizeof(line));
                    int count = -1;
                    if ((size_t)l < counts.size()) {
                        GlobalVariable *gv = counts[l];
                        if (gv) {
                            int *p = (int*)jl_ExecutionEngine->getPointerToGlobal(gv);
                            count = *p;
                        }
                    }
                    outf.width(9);
                    if (count == -1)
                        outf<<'-';
                    else
                        outf<<count;
                    outf.width(0);
                    outf<<" "<<line<<std::endl;
                    l++;
                }
                outf.close();
                inf.close();
            }
        }
    }
}

#ifndef _OS_WINDOWS_
// disabled pending system image backtrace support on Windows
typedef std::map<size_t, FuncInfo, revcomp> FuncInfoMap;
extern "C" void jl_dump_linedebug_info() {
    FuncInfoMap info = jl_jit_events->getMap();
    FuncInfoMap::iterator infoiter = info.begin();
    std::vector<JITEvent_EmittedFunctionDetails::LineStart>::iterator lineiter = (*infoiter).second.lines.begin();

    Type *li_types[2] = {T_size, T_size};
    StructType *T_lineinfo = StructType::get(jl_LLVMContext, ArrayRef<Type *>(std::vector<Type *>(li_types, li_types+2)), true);

    std::vector<Constant *> funcinfo_array;
    funcinfo_array.push_back( ConstantInt::get(T_size, 0) );

    for (; infoiter != info.end(); infoiter++) {
        std::vector<Constant*> functionlines;

        // get the base address for offset calculation
        size_t fptr = (size_t)(*infoiter).first;

        lineiter = (*infoiter).second.lines.begin();
        JITEvent_EmittedFunctionDetails::LineStart prev = *lineiter;

        // loop over the EmittedFunctionDetails vector
        while (lineiter != (*infoiter).second.lines.end()) {
            // store the individual {offset, line} entries
            Constant* tmpline[2] = { ConstantInt::get(T_size, (*lineiter).Address - fptr),
                                     ConstantInt::get(T_size, (*lineiter).Loc.getLine()) };
            Constant *lineinfo = ConstantStruct::get(T_lineinfo, makeArrayRef(tmpline));
            functionlines.push_back(lineinfo);
            lineiter++;
        }

        DISubprogram debugscope =
            DISubprogram(prev.Loc.getScope((*infoiter).second.func->getContext()));

        // store function pointer, name and filename, length, number of line entries, and then array of line mappings
        Constant *info_data[6] = { ConstantExpr::getBitCast( const_cast<Function *>((*infoiter).second.func), T_psize),
                                   ConstantDataArray::getString( jl_LLVMContext, StringRef(debugscope.getName().str())),
                                   ConstantDataArray::getString( jl_LLVMContext, StringRef(debugscope.getFilename().str())),
                                   ConstantInt::get(T_size, (*infoiter).second.lengthAdr),
                                   ConstantInt::get(T_size, functionlines.size()),
                                   ConstantArray::get(ArrayType::get(T_lineinfo, functionlines.size()), ArrayRef<Constant *>(functionlines))
                                 };
        Constant *st = ConstantStruct::getAnon(jl_LLVMContext, ArrayRef<Constant *>(info_data), true);
        funcinfo_array.push_back( st );
    }
    // set first element to the total number of FuncInfo entries
    funcinfo_array[0] = ConstantInt::get(T_size, funcinfo_array.size() - 1);
    Constant *st_lineinfo = ConstantStruct::getAnon( jl_LLVMContext, ArrayRef<Constant*>(funcinfo_array), true );
    new GlobalVariable(
            *jl_Module,
            st_lineinfo->getType(),
            true,
            GlobalVariable::ExternalLinkage,
            st_lineinfo,
            "jl_linedebug_info");
}

extern "C" void jl_restore_linedebug_info(uv_lib_t *handle) {
    uintptr_t *infoptr = (uintptr_t*)jl_dlsym(handle, const_cast<char*>("jl_linedebug_info"));
    size_t funccount = (size_t)(*infoptr++);

    for (size_t i = 0; i < funccount; i++) {
        // loop over function info entries
        uintptr_t fptr = (*infoptr++);
        char *name = (char*)infoptr;
        infoptr = (uintptr_t*)(((char*)infoptr) + strlen( (const char*)infoptr) + 1);
        char *filename = (char*)infoptr;
        infoptr = (uintptr_t*)(((char*)infoptr) + strlen( (const char*)infoptr) + 1);
        size_t lengthAdr = (*infoptr++);
        size_t numel = (*infoptr++);

        std::vector<JITEvent_EmittedFunctionDetails::LineStart> linestarts;

        // dummy element for the MDNode, which we need so that the DebucLoc keeps info
        SmallVector <Value *, 1> tmpelt;

        for (size_t j = 0; j < numel; j++) {
            // loop over individual {offset, line} entries
            uintptr_t offset = (*infoptr++);
            uintptr_t line = (*infoptr++);
            JITEvent_EmittedFunctionDetails::LineStart linestart =
                { (uintptr_t)fptr + offset, DebugLoc::get( line, 0, MDNode::get(jl_LLVMContext, tmpelt) ) };
            linestarts.push_back(linestart);
        }
        FuncInfo info = { NULL, lengthAdr, std::string(name), std::string(filename), linestarts };
        jl_jit_events->getMap()[(size_t)fptr] = info;
    }
}
#endif
