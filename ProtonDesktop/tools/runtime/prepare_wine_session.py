#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checked, build-local lifecycle overlay for the pinned Wine64 UWP host.

The original kernel, VFS, ELF loader and X11 setup remain upstream code. Inputs
may include the earlier UWP capability profile; external/ is never changed.
"""
from pathlib import Path
import argparse


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"Pinned runtime edit must match exactly once: {before[:100]!r}")
    return text.replace(before, after, 1)


def startup(cpp, header):
    cpp = once(cpp, "char* base = getcwd(curdir, sizeof(curdir));",
        "char* base = platform_getcwd(curdir, sizeof(curdir));")
    begin = cpp.index("bool StartUpArgs::apply() {")
    end = cpp.index("    x11_init();", begin) + len("    x11_init();")
    prepare = cpp[begin:end]
    prepare = once(prepare, "bool StartUpArgs::apply() {",
        "bool StartUpArgs::prepareProtonSession(std::vector<std::shared_ptr<FsZip>>& openZips) {")
    prepare = once(prepare, "    std::vector<std::shared_ptr<FsZip>> openZips;", "    openZips.clear();")
    prepare = once(prepare, '        fsZip->init(zip, B(""));',
        '        if (!fsZip->init(zip, B(""))) return false;')
    prepare = once(prepare, "                fsZip->init(info.nativePath, info.localPath);",
        "                if (!fsZip->init(info.nativePath, info.localPath)) return false;")
    # A typed guest command must never become a host-path auto-mount/fallback.
    a = prepare.index("    if (this->args.size()==0) {")
    b = prepare.index("    if (this->sdlFullScreen!=FULLSCREEN_NOTSET", a)
    prepare = prepare[:a] + prepare[b:]
    # Keep caller environment authoritative; upstream defaults fill absent keys.
    prepare = once(prepare, "    KSystem::init();", """    auto addDefault = [this](const BString& value) {
        int equals = value.indexOf('=');
        BString key = value.substr(0, equals + 1);
        for (const auto& existing : envValues) if (existing.startsWith(key)) return;
        envValues.push_back(value);
    };
    KSystem::init();""")
    for original in [
        'envValues.push_back(B("HOME=/home/username"));',
        'envValues.push_back(B("LOGNAME=username"));',
        'envValues.push_back(B("USERNAME=username"));',
        'envValues.push_back(B("USER=username"));',
        'envValues.push_back("PWD="+this->workingDir);',
        'envValues.push_back(B("DISPLAY=:0"));',
        'envValues.push_back(B("WINE_FAKE_WAIT_VBLANK=60"));',
        'envValues.push_back(B("PATH=/bin:/usr/bin:/usr/local/bin:/sbin:/usr/sbin:/usr/local/sbin"));',
        'envValues.push_back(B("PATH=/bin:/usr/bin:/usr/local/bin"));',
    ]:
        prepare = once(prepare, original, original.replace("envValues.push_back", "addDefault"))
    prepare += "\n    return true;\n}\n\n"
    # CLI/default-resource discovery is not part of the native typed launcher.
    # Retaining this method would introduce an unsupported Platform import.
    resource_begin = cpp.index("bool StartUpArgs::loadDefaultResource(")
    resource_end = cpp.index("bool StartUpArgs::parseStartupArgs(", resource_begin)
    cpp = cpp[:resource_begin] + cpp[resource_end:]
    cpp = cpp[:begin] + prepare + cpp[begin:]
    header = once(header, "class StartUpArgs {", "class FsZip;\n\nclass StartUpArgs {")
    header = once(header, "    bool apply();", "    bool apply();\n    bool prepareProtonSession(std::vector<std::shared_ptr<FsZip>>& openZips);")
    return cpp, header


ROLLBACK_HELPER = r'''
// Only for a NEW thread whose host worker was not created. Never call on a
// scheduled/current guest. Remove it before returning a real launch/clone error.
static void protonRollbackUnscheduledThread(KThread* thread) {
    KProcessPtr owner = thread->process;
    thread->clear_child_tid = 0;
#ifdef BOXEDWINE_GUEST_X64
    thread->clear_child_tid64 = 0;
#endif
    ChangeThread bind(thread);
    owner->deleteThread(thread);
}

static bool protonScheduleNewThread(KThread* thread) {
    try { scheduleThread(thread); return true; }
    catch (const std::exception& error) {
        klog_fmt("ProtonDesktop: guest worker creation failed: %s", error.what());
    } catch (...) { klog("ProtonDesktop: guest worker creation failed"); }
    protonRollbackUnscheduledThread(thread);
    return false;
}

'''


def process(cpp):
    cpp = once(cpp, "#include <stdlib.h>", "#include <stdlib.h>\n#include <exception>")
    marker = "KThread* KProcess::startProcess("
    start = cpp.index(marker)
    cpp = cpp[:start] + ROLLBACK_HELPER + cpp[start:]
    start = cpp.index(marker)
    end = cpp.index("U32 KProcess::exit(", start)
    method = cpp[start:end]
    method = once(method, "    KThread::setCurrentThread(thread);", "    ChangeThread bindInitialThread(thread);")
    method = once(method, "    if (!openNode) {\n        return nullptr;\n    }",
        "    if (!openNode) {\n        protonRollbackUnscheduledThread(thread);\n        return nullptr;\n    }")
    method = once(method, "        scheduleThread(thread);",
        "        if (!protonScheduleNewThread(thread)) return nullptr;")
    method = once(method, "    }\n    return thread;\n}",
        "        protonRollbackUnscheduledThread(thread);\n        return nullptr;\n    }\n    return thread;\n}")
    cpp = cpp[:start] + method + cpp[end:]
    # All clone/fork call sites must return an actual guest errno on host spawn
    # failure. A swallowed exception must never leave an unscheduled phantom.
    # Preserve original PARENT_SETTID memory when a failed launch rolls back.
    clone_begin = cpp.index("U32 KProcess::clone(KThread*")
    clone_end = cpp.index("#ifdef BOXEDWINE_GUEST_X64\nU32 KProcess::clone64", clone_begin)
    clone = cpp[clone_begin:clone_end]
    clone = once(clone, "    flags &= ~CSIGNAL;", "    const bool protonSharedVm = (flags & K_CLONE_VM) != 0;\n    U32 protonOldParentTid = (ptid && (flags & K_CLONE_PARENT_SETTID)) ? memory->readd(ptid) : 0;\n    U32 protonOldChildTid = (ctid && (flags & K_CLONE_CHILD_SETTID)) ? memory->readd(ctid) : 0;\n    flags &= ~CSIGNAL;")
    failure = """const U32 protonChildId = newThread->id;
                if (!protonScheduleNewThread(newThread)) {
                    if (ptid && (flags & K_CLONE_PARENT_SETTID)) memory->writed(ptid, protonOldParentTid);
                    if (ctid && (flags & K_CLONE_CHILD_SETTID) && protonSharedVm) memory->writed(ctid, protonOldChildTid);
                    return (U32)-K_EAGAIN;
                }"""
    if clone.count("scheduleThread(newThread);") != 3:
        raise ValueError("Pinned clone32 schedule count changed")
    clone = clone.replace("scheduleThread(newThread);", failure)
    clone = once(clone, "        return newThread->id;", "        return protonChildId;")
    cpp = cpp[:clone_begin] + clone + cpp[clone_end:]
    clone_begin = cpp.index("U32 KProcess::clone64(")
    fork_begin = cpp.index("U32 KProcess::forkProcess64(", clone_begin)
    clone = cpp[clone_begin:fork_begin]
    clone = once(clone, "    KThread* newThread = this->createThread();", "    U32 protonOldParentTid = (ptid && (flags & K_CLONE_PARENT_SETTID)) ? this->memory64->readd(ptid) : 0;\n    U32 protonOldChildTid = (ctid && (flags & K_CLONE_CHILD_SETTID)) ? this->memory64->readd(ctid) : 0;\n    KThread* newThread = this->createThread();")
    clone = once(clone, "    scheduleThread(newThread);", """    const U32 protonChildId = newThread->id;
    if (!protonScheduleNewThread(newThread)) {
        if (ptid && (flags & K_CLONE_PARENT_SETTID)) this->memory64->writed(ptid, protonOldParentTid);
        if (ctid && (flags & K_CLONE_CHILD_SETTID)) this->memory64->writed(ctid, protonOldChildTid);
        return (U32)-K_EAGAIN;
    }""")
    clone = once(clone, "    return newThread->id;", "    return protonChildId;")
    cpp = cpp[:clone_begin] + clone + cpp[fork_begin:]
    fork_begin = cpp.index("U32 KProcess::forkProcess64(")
    fork_end = cpp.index("void KProcess::killAllThreads(", fork_begin)
    fork = cpp[fork_begin:fork_end]
    fork = once(fork, "    if (flags & K_CLONE_PARENT_SETTID) {", "    U32 protonOldParentTid = (ptid && (flags & K_CLONE_PARENT_SETTID)) ? this->memory64->readd(ptid) : 0;\n    if (flags & K_CLONE_PARENT_SETTID) {")
    fork = once(fork, "    scheduleThread(newThread);", """    if (!protonScheduleNewThread(newThread)) {
        if (ptid && (flags & K_CLONE_PARENT_SETTID)) this->memory64->writed(ptid, protonOldParentTid);
        return (U32)-K_EAGAIN;
    }""")
    cpp = cpp[:fork_begin] + fork + cpp[fork_end:]
    # The pinned destructor only frees the 32-bit facade. The main CPU64 and
    # its address space have process lifetime, including a failed ELF launch.
    destructor_begin = cpp.index("KProcess::~KProcess() {")
    destructor_end = cpp.index("void KProcess::cleanupProcess()", destructor_begin)
    destructor = cpp[destructor_begin:destructor_end]
    destructor = once(destructor, "    if (memory) {\n        delete memory;\n    }", """    if (memory) {
        delete memory;
    }
#ifdef BOXEDWINE_GUEST_X64
    delete retiredCpu64;
    delete retiredMemory64;
    delete cpu64;
    delete memory64;
#endif""")
    cpp = cpp[:destructor_begin] + destructor + cpp[destructor_end:]
    return cpp


def kernel(cpp, header):
    start = cpp.index("void KSystem::destroy() {")
    stop = cpp.index("    KSystem::procNode = nullptr;", start)
    stop_method = cpp[start:stop].replace("void KSystem::destroy() {", "void KSystem::protonStopGuestProcesses() {", 1) + "}\n\n"
    cpp = cpp[:start] + stop_method + cpp[start:]
    header = once(header, "\tstatic void destroy();", "\tstatic void destroy();\n    static void protonStopGuestProcesses();")
    return cpp, header


def memory(cpp, header):
    # Fork snapshots must retain the allocator's address-space reservations as
    # well as committed bytes. Otherwise the first child mmap(NULL) starts at
    # the base again and zeroes inherited dynamic-linker data.
    header = once(header, "    BOXEDWINE_MUTEX mmapMutex;", "    mutable BOXEDWINE_MUTEX mmapMutex;")
    cpp = once(cpp,
        "    std::lock_guard<std::recursive_mutex> lockFrom(from->pagesMutex);",
        "    std::lock_guard<std::recursive_mutex> lockMmapFrom(from->mmapMutex);\n"
        "    std::lock_guard<std::recursive_mutex> lockMmapThis(mmapMutex);\n"
        "    std::lock_guard<std::recursive_mutex> lockFrom(from->pagesMutex);")
    cpp = once(cpp, "#endif\n    pages.clear();\n    pages.reserve(from->pages.size());",
        "#endif\n    ranges = from->ranges;\n    mmapNext = from->mmapNext;\n"
        "    pages.clear();\n    pages.reserve(from->pages.size());")
    # Shared file pages belong to the shared registry. Fork must retain their
    # backing buffers; only private pages get independent copied contents.
    cpp = once(cpp, "        if (kv.second->committed()) {\n",
        "        if (kv.second->dataShared) {\n"
        "            copy->adoptShared(kv.second->data);\n"
        "        } else if (kv.second->committed()) {\n")
    return cpp, header


def syscalls(cpp):
    # A NULL-address shared mapping reserves an address first. That reservation
    # must not turn Wine's shared server clock into a private copy.
    cpp = once(cpp,
        "if (serverSection && (flags & K_MAP_SHARED) && kfile->openFile->node && !reserved) {",
        "if (serverSection && (flags & K_MAP_SHARED) && kfile->openFile->node) {")
    # The guest uses Linux x86-64 struct flock, whereas KProcess::fcntrl reads
    # packed i386 flock64 from its 32-bit scratch address space. Preserve the
    # actual 64-bit offsets and leave guest padding untouched on copy-out.
    cpp = once(cpp, """            // Lock command: marshal the 24-byte 64-bit flock into scratch.
            const U32 FLOCK64_BYTES = 24;
            U32 scratch = bounceSockaddrTo32(cpu, 0, 0, nullptr);
            if (!scratch) { ret = (U64)-K_EFAULT; break; }
            {
                U8 tmp[FLOCK64_BYTES];
                cpu->memory->memcpyFromGuest(tmp, a3, FLOCK64_BYTES);
                cpu->thread->memory->memcpy(scratch, tmp, FLOCK64_BYTES);
            }
""", """            // Linux x86-64 struct flock is 32 bytes (start@8, len@16,
            // pid@24); the i386 flock64 scratch is packed to 24 bytes
            // (start@4, len@12, pid@20). Marshal fields, never raw padding.
            U32 scratch = bounceSockaddrTo32(cpu, 0, 0, nullptr);
            if (!scratch) { ret = (U64)-K_EFAULT; break; }
            cpu->thread->memory->writew(scratch + 0, cpu->memory->readw(a3 + 0));
            cpu->thread->memory->writew(scratch + 2, cpu->memory->readw(a3 + 2));
            cpu->thread->memory->writeq(scratch + 4, cpu->memory->readq(a3 + 8));
            cpu->thread->memory->writeq(scratch + 12, cpu->memory->readq(a3 + 16));
            cpu->thread->memory->writed(scratch + 20, cpu->memory->readd(a3 + 24));
""")
    cpp = once(cpp, """            if (cmd == K_F_GETLK || cmd == K_F_GETLK64) {
                U8 tmp[FLOCK64_BYTES];
                cpu->thread->memory->memcpy(tmp, scratch, FLOCK64_BYTES);
                cpu->memory->memcpyToGuest(a3, tmp, FLOCK64_BYTES);
            }
""", """            if ((cmd == K_F_GETLK || cmd == K_F_GETLK64) && (S64)ret >= 0) {
                cpu->memory->writew(a3 + 0, cpu->thread->memory->readw(scratch + 0));
                cpu->memory->writew(a3 + 2, cpu->thread->memory->readw(scratch + 2));
                cpu->memory->writeq(a3 + 8, cpu->thread->memory->readq(scratch + 4));
                cpu->memory->writeq(a3 + 16, cpu->thread->memory->readq(scratch + 12));
                cpu->memory->writed(a3 + 24, cpu->thread->memory->readd(scratch + 20));
            }
""")
    return cpp


def prepare(source, profile, output):
    def read(relative, profiled=False):
        origin = profile / relative if profiled and (profile / relative).exists() else source / relative
        return origin.read_text().replace("\r\n", "\n")
    def write(relative, text):
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    cpp, header = startup(read("source/sdl/startupArgs.cpp", True), read("source/sdl/startupArgs.h"))
    write("source/sdl/startupArgs.cpp", cpp)
    write("source/sdl/startupArgs.h", header)
    write("source/kernel/kprocess.cpp", process(read("source/kernel/kprocess.cpp")))
    cpp, header = kernel(read("source/kernel/ksystem.cpp", True), read("include/ksystem.h"))
    write("source/kernel/ksystem.cpp", cpp)
    write("include/ksystem.h", header)
    cpp, header = memory(read("source/kernel/kmemory64.cpp"), read("include/kmemory64.h"))
    write("source/kernel/kmemory64.cpp", cpp)
    write("include/kmemory64.h", header)
    write("source/kernel/syscall64.cpp", syscalls(read("source/kernel/syscall64.cpp")))
    loop = read("source/sdl/multiThreaded/threadedMainloop.cpp")
    loop = once(loop, "static thread_local bool isMainThread;", "static thread_local bool isMainThread;\nvoid protonSetSdlOwnerThread(bool value) { isMainThread = value; }")
    write("source/sdl/multiThreaded/threadedMainloop.cpp", loop)
    input_cpp = read("platform/sdl/knativeinputSDL.cpp")
    first = input_cpp.index("    if (e->type == SDL_QUIT) {", input_cpp.index("bool KNativeInputSDL::handlSdlEvent"))
    # Select the non-recorder QUIT block, by its exact PID10-body prefix.
    original = "    if (e->type == SDL_QUIT) {\n        KThread::setCurrentThread(nullptr);"
    input_cpp = once(input_cpp, original, "    if (e->type == SDL_QUIT) {\n        return false; // Managed WineSession owns ALL-process stop and the join barrier.\n        KThread::setCurrentThread(nullptr);")
    write("platform/sdl/knativeinputSDL.cpp", input_cpp)
    present = read("source/x11wire/xwirepresentSDL.cpp")
    first = present.index("        if (!haveFrame && KSystem::bootProgressPercent >= 0) {")
    last = present.index("        // App-switch canvas wipe:", first)
    present = present[:first] + "        // Native Wine profile: no synthetic boot bitmap or inferred guest frame.\n\n" + present[last:]
    present = once(present, "bool isMainthread();", "bool isMainthread();\nvoid protonWineGuestFrameSubmitted(unsigned width, unsigned height);")
    present = once(present, "            screen->putBitsOnWnd(0, frame.data(), 32, (U32)fw * 4, 0, 0, fw, fh, nullptr, true);",
        "            protonWineGuestFrameSubmitted(fw, fh);\n            screen->putBitsOnWnd(0, frame.data(), 32, (U32)fw * 4, 0, 0, fw, fh, nullptr, true);")
    write("source/x11wire/xwirepresentSDL.cpp", present)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source.resolve(), args.profile.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
