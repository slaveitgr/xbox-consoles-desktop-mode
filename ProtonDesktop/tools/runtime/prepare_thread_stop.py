#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checked multi-threaded cancellation overlay, after WineSession/owned workers.

Only disposable output is written. Existing profile edits always win over the
pristine pin; modified translation units keep those edits. Every quoted include
in the emitted files is rebound to its exact profile/pristine/generated header,
so a copied .cpp cannot silently select stale adjacent headers.

Consumers must replace every manifest source_file in their build and put
output/include BEFORE the profile/pristine include directories for ALL core
translation units. This is an opt-in MT profile, not a Wine shutdown proof.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

PIN = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"
ROOT = Path(__file__).resolve().parents[2]
READ_COUNTS = {
    "source/emulation/cpu/common/cpu.cpp": 2,
    "source/emulation/cpu/cpu64.cpp": 2,
    "source/emulation/cpu/normal/normalCPU.cpp": 1,
    "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp": 5,
    "source/kernel/knetlink.cpp": 3,
    "source/kernel/kpoll.cpp": 1,
    "source/kernel/kprocess.cpp": 1,
    "source/kernel/kscheduler.cpp": 1,
    "source/kernel/ksignal.cpp": 1,
    "source/kernel/ksystem.cpp": 2,
    "source/kernel/kthread.cpp": 4,
    "source/kernel/ktimer.cpp": 1,
    "source/kernel/kunixsocket.cpp": 6,
    "source/kernel/syscall.cpp": 2,
    "source/kernel/syscall64.cpp": 1,
    "source/sdl/emscripten/wine64session.cpp": 0,
}
SOURCE_FILES = tuple(READ_COUNTS) + ("source/util/synchronization.cpp",)
HEADER_FILES = ("include/kthread.h", "include/kprocess.h", "include/boxedwine.h",
                "source/emulation/cpu/normal/normal_other.h")
POINTER = r"((?:(?:this|cpu)->)?thread|this|target|currentThread|KThread::currentThread\(\))"
TERM = re.compile(POINTER + r"->terminating\b")
WAIT = re.compile(POINTER + r"->waitingCond\b")
QUOTED_INCLUDE = re.compile(r'^(\s*#\s*include\s*)"([^"\n]+)"', re.MULTILINE)


def replace(text, before, after, count=1):
    actual = text.count(before)
    if actual != count:
        raise ValueError(f"Pinned stop edit expected {count} occurrences, got {actual}: {before[:100]!r}")
    return text.replace(before, after)


def regex_replace(pattern, replacement, text, count, label):
    result, actual = pattern.subn(replacement, text)
    if actual != count:
        raise ValueError(f"Pinned stop {label} count changed: expected {count}, got {actual}")
    return result


THREAD_METHODS = """    BOXEDWINE_MUTEX waitingCondSync;
    // Stop flag and condition registration share this existing mutex. Callers
    // never retain it while acquiring a condition's mutex or joining a worker.
    bool protonIsTerminating() {
        std::lock_guard<std::recursive_mutex> guard(waitingCondSync);
        return terminating;
    }
    BOXEDWINE_CONDITION protonRequestTermination() {
        std::lock_guard<std::recursive_mutex> guard(waitingCondSync);
        terminating = true;
        return waitingCond;
    }
    BOXEDWINE_CONDITION protonWaitingCondition() {
        std::lock_guard<std::recursive_mutex> guard(waitingCondSync);
        return waitingCond;
    }
    bool protonEnterWait(const BOXEDWINE_CONDITION& condition) {
        std::lock_guard<std::recursive_mutex> guard(waitingCondSync);
        if (terminating) return false;
        waitingCond = condition;
        return true;
    }
    void protonLeaveWait() {
        std::lock_guard<std::recursive_mutex> guard(waitingCondSync);
        waitingCond.reset();
    }"""

NORMAL_STOP = """    KThread* thread = process->getThreadById(threadId);
    if (thread) {
        thread->terminating = true;

        const std::shared_ptr<BoxedWineCondition> cond = thread->waitingCond;

        // wake up the thread if it is waiting
        if (cond) {
            cond->lock();
            cond->signal();
            cond->unlock();
        }
    }
"""
NORMAL_STOP_FIXED = """    // Resolve/mark while the process still owns the KThread. Only a strong
    // condition reference escapes that lock, never an unpinned thread pointer.
    const auto cond = process->protonRequestThreadTermination(threadId);
    if (cond) {
        cond->lock();
        // Multiple threads can share a socket/futex/parent condition. Waking
        // one arbitrary waiter need not wake the thread being terminated.
        cond->signalAll();
        cond->unlock();
    }
"""
PROCESS_REQUEST = """BOXEDWINE_CONDITION KProcess::protonRequestThreadTermination(U32 threadId) {
    std::lock_guard<std::recursive_mutex> guard(threadsMutex);
    KThread* thread = threads[threadId];
    return thread ? thread->protonRequestTermination() : nullptr;
}

"""


def transform(relative, text):
    if "protonIsTerminating" in text or "PROTONDESKTOP_THREAD_STOP_OVERLAY" in text:
        raise ValueError(f"Thread-stop overlay already applied: {relative}")
    if relative == "include/kthread.h":
        text = replace(text, "#define __KTHREAD_H__", """#define __KTHREAD_H__
#define PROTONDESKTOP_THREAD_STOP_OVERLAY 1
#ifndef BOXEDWINE_MULTI_THREADED
#error The checked ProtonDesktop thread-stop overlay requires BOXEDWINE_MULTI_THREADED.
#endif""")
        return replace(text, "    BOXEDWINE_MUTEX waitingCondSync;", THREAD_METHODS)
    if relative == "include/kprocess.h":
        return replace(text, "    void killAllThreads(KThread* exceptThisOne = nullptr);",
                       "    void killAllThreads(KThread* exceptThisOne = nullptr);\n"
                       "    BOXEDWINE_CONDITION protonRequestThreadTermination(U32 threadId);")
    if relative == "include/boxedwine.h":
        # This umbrella must be emitted too: its quoted kthread/kprocess header
        # edges otherwise bind the OLD header in the profile's own directory.
        replace(text, '#include "kthread.h"', '#include "kthread.h"')
        replace(text, '#include "kprocess.h"', '#include "kprocess.h"')
        return text
    if relative == "source/util/synchronization.cpp":
        for call in ("this->c.wait(lock);",
                     "this->c.wait_for(lock, std::chrono::milliseconds(KSystem::emulatedMilliesToHost(ms)));"):
            before = """    if (thread) {
        thread->waitingCond = shared_from_this();
    }
    """ + call + """
    if (thread) {
        thread->waitingCond = nullptr;
    }"""
            after = """    if (thread && !thread->protonEnterWait(shared_from_this())) return;
    """ + call + "\n    if (thread) thread->protonLeaveWait();"
            text = replace(text, before, after)
        return text
    if relative == "source/emulation/cpu/normal/normalPlatformMultiThreaded.cpp":
        if text.count("protondesktop::uwp::SpawnGuestWorker") != 1:
            raise ValueError("Owned-worker overlay must precede the thread-stop overlay")
        text = replace(text, NORMAL_STOP, NORMAL_STOP_FIXED)
        text = replace(text, "    thread->terminating = true;", "    (void)thread->protonRequestTermination();")
    elif relative == "source/kernel/kprocess.cpp":
        if "protonRollbackUnscheduledThread" not in text:
            raise ValueError("WineSession lifecycle overlay must precede the thread-stop overlay")
        text = replace(text, "void KProcess::killAllThreads(KThread* exceptThisThread) {",
                       PROCESS_REQUEST + "void KProcess::killAllThreads(KThread* exceptThisThread) {")
    elif relative in ("source/kernel/kscheduler.cpp", "source/sdl/emscripten/wine64session.cpp"):
        text = replace(text, "thread->terminating = true;", "(void)thread->protonRequestTermination();")
    if relative == "source/emulation/cpu/normal/normal_other.h":
        text = regex_replace(TERM, r"\1->protonIsTerminating()", text, 1, relative + " flag")
        return regex_replace(WAIT, r"\1->protonWaitingCondition()", text, 1, relative + " condition")
    text = regex_replace(TERM, r"\1->protonIsTerminating()", text, READ_COUNTS[relative], relative + " flag")
    if relative == "source/kernel/kthread.cpp":
        # Remaining direct accesses here are inside !MULTI_THREADED branches.
        for before, after in (
            ("BOXEDWINE_CONDITION cond = waitingCond;", "BOXEDWINE_CONDITION cond = protonWaitingCondition();"),
            ("if (waitingCond) {", "if (protonWaitingCondition()) {"),
            ("BOXEDWINE_CONDITION c = this->waitingCond;", "BOXEDWINE_CONDITION c = this->protonWaitingCondition();"),
            ("BOXEDWINE_CONDITION cond = this->waitingCond;", "BOXEDWINE_CONDITION cond = this->protonWaitingCondition();"),
        ):
            text = replace(text, before, after)
    else:
        counts = {"source/kernel/kprocess.cpp": 5, "source/kernel/kscheduler.cpp": 2,
                  "source/sdl/emscripten/wine64session.cpp": 1}
        text = regex_replace(WAIT, r"\1->protonWaitingCondition()", text, counts.get(relative, 0),
                             relative + " condition")
    if relative == "source/kernel/kprocess.cpp":
        text = replace(text,
            '        if (thread->protonWaitingCondition()) {            \n'
            '            klog_fmt("  thread %X WAITING %s", thread->id, thread->protonWaitingCondition()->name.c_str());',
            '        if (const auto condition = thread->protonWaitingCondition()) {\n'
            '            klog_fmt("  thread %X WAITING %s", thread->id, condition->name.c_str());')
    return text


def prepare(source: Path, profile: Path, output: Path):
    source, profile, output = source.resolve(), profile.resolve(), output.resolve()
    if output == source or output.is_relative_to(source):
        raise ValueError("The pristine source tree cannot be the overlay output")
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if pin != PIN or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise ValueError("The exact pristine pinned source tree is required")
    prepared, origins, inputs = {}, {}, {}
    for relative in SOURCE_FILES + HEADER_FILES:
        origin = profile / relative if (profile / relative).is_file() else source / relative
        data = origin.read_bytes()
        origins[relative] = origin
        inputs[relative] = {"origin": "profile" if origin.is_relative_to(profile) else "pristine",
                            "sha256": hashlib.sha256(data).hexdigest()}
        prepared[relative] = transform(relative, data.decode("utf-8").replace("\r\n", "\n"))
        # Only these exact accesses remain, all in !MULTI_THREADED branches.
        # A newly added pointer expression must not silently bypass the helpers.
        remaining = len(re.findall(r"->(?:terminating|waitingCond)\b", prepared[relative]))
        expected = {"source/kernel/kthread.cpp": 5, "source/util/synchronization.cpp": 4}.get(relative, 0)
        if remaining != expected:
            raise ValueError(f"Unredirected stop-field access count changed in {relative}: {remaining}, expected {expected}")
    edges = {}

    def logical(path):
        for base in (profile, source):
            if path.is_relative_to(base):
                return path.relative_to(base).as_posix()
        return None

    def rebase(relative, text):
        local_edges = []

        def bind(match):
            name = match.group(2)
            candidate = Path(name)
            candidates = ([candidate] if candidate.is_absolute() else
                          [origins[relative].parent / name,
                           profile / Path(relative).parent / name,
                           source / Path(relative).parent / name,
                           profile / "include" / name, source / "include" / name, ROOT / name])
            selected = next((path.resolve() for path in candidates if path.is_file()), None)
            if selected is None:
                if name.startswith(("../", "./")):
                    raise ValueError(f"Cannot resolve pinned quoted relative include {relative}: {name}")
                # E.g. Apple's TargetConditionals.h in an inactive MAC branch.
                # Preserve the original compiler/platform lookup, explicitly.
                local_edges.append({"include": name, "resolution": "external-conditional"})
                return match.group(0)
            target = logical(selected)
            if target in prepared:
                selected = output / target
                resolution = "generated"
            else:
                resolution = "profile" if selected.is_relative_to(profile) else "pristine-or-project"
            local_edges.append({"include": name, "target": selected.as_posix(), "resolution": resolution})
            return match.group(1) + '"' + selected.as_posix() + '"'

        result = QUOTED_INCLUDE.sub(bind, text)
        edges[relative] = local_edges
        return result

    # Check/resolve every input before writing even when applying in-place.
    emitted = {relative: rebase(relative, text) for relative, text in prepared.items()}
    output.mkdir(parents=True, exist_ok=True)
    for relative, text in emitted.items():
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    manifest = {"source_pin": PIN, "profile": str(profile), "output": str(output),
                "source_files": list(SOURCE_FILES), "header_files": list(HEADER_FILES),
                "required_include_first": str(output / "include"), "requires_multi_threaded": True,
                "data_fields_added": False, "full_wine_shutdown_qualified": False,
                "inputs": inputs, "quoted_include_edges": edges}
    (output / "thread-stop-overlay.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.source, args.profile, args.output)
    print(json.dumps({key: result[key] for key in ("source_pin", "source_files", "header_files",
                                                 "required_include_first", "data_fields_added")}, indent=2))


if __name__ == "__main__":
    main()
