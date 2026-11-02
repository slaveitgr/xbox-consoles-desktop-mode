#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stage root-coordinate input routing after the opt-in XWire desktop overlay.

All three XWire translation units must use the complete shadow headers together:
XWireConnection gains a protocol mutex, changing its layout. This helper performs
all anchor checks before writing; it never edits the pristine source checkout.
Legacy single-app input remains the default until protonConfigureXWireDesktop.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

PIN = "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"
REQUIRED_TUS = ("xwireconnection.cpp", "xwireserver.cpp", "xwirepresentSDL.cpp")
REQUIRED_HEADERS = ("xwireconnection.h", "xwireserver.h", "xwirepresent.h", "xwirefont.h")


def once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("XWire input anchor mismatch: " + repr(before[:100]))
    return text.replace(before, after, 1)


def prepare_xwire_input(overlay_root: Path, source_root: Path = None) -> dict:
    directory = Path(overlay_root).resolve() / "source/x11wire"
    if source_root is not None:
        source = Path(source_root).resolve()
        if directory.is_relative_to(source):
            raise ValueError("The pristine source cannot be an input overlay output")
        pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        changed = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True)
        if pin != PIN or changed:
            raise ValueError("The exact pristine pinned Boxedwine64 checkout is required")
    for name in REQUIRED_TUS + REQUIRED_HEADERS:
        if not (directory / name).is_file():
            raise RuntimeError("Missing complete desktop source/header overlay: " + name)
    if "static void protonComposeDesktop(" not in (directory / "xwireserver.cpp").read_text():
        raise RuntimeError("Run prepare_xwire_desktop before preparing input")
    if "Native Wine profile: no synthetic boot bitmap" not in (directory / "xwirepresentSDL.cpp").read_text():
        raise RuntimeError("The production synthetic-disabled Wine presenter is required")
    s = directory
    h=(s/'xwireconnection.h').read_text()
    c=(s/'xwireconnection.cpp').read_text()
    server=(s/'xwireserver.cpp').read_text()
    h=once(h,'    void onData();','''    void onData();
    bool ownsDesktopWindow(uint32_t target);
        void deliverDesktopInput(uint32_t target,const XWireInputEvent& event,int originX,int originY,bool focus);
        void leaveDesktopPointer(uint32_t target,int rootX,int rootY,int originX,int originY);
        void notifyDesktopFocus(uint32_t window,bool focused);''')
    h=once(h,'    bool handshakeDone = false;',
        '    std::recursive_mutex protonProtocolMutex;\n    bool handshakeDone = false;')
    h=once(h,'#include <memory>','#include <memory>\n#include <mutex>')
    prototypes='''
    extern bool protonQueryXWireDesktop(unsigned&,unsigned&);
    uint32_t protonDesktopGetInputFocus();
    bool protonDesktopSetInputFocus(uint32_t);
    bool protonDesktopPointerInfo(uint32_t,int,int,uint32_t&,int&,int&,uint32_t&);
    bool protonDesktopRootOrigin(uint32_t,int&,int&);
    '''
    c=once(c,'XWirePresentSink* g_xwirePresentSink = nullptr;',
        'XWirePresentSink* g_xwirePresentSink = nullptr;\n'+prototypes)
    lock='''    unsigned protonWidth,protonHeight;
        std::unique_lock<std::recursive_mutex> protonLock(protonProtocolMutex,std::defer_lock);
        if(protonQueryXWireDesktop(protonWidth,protonHeight))protonLock.lock();
    '''
    for signature in ['void XWireConnection::onData() {',
                      'void XWireConnection::writeToClient(const void* data, uint32_t len) {',
                      'void XWireConnection::flushReplies() {',
                      'void XWireConnection::sendSelectionClear(uint32_t ownerWindow, const std::string& selectionName) {']:
        c=once(c,signature,signature+'\n'+lock)
    c=once(c,'void XWireConnection::deliverInputEvents() {','''void XWireConnection::deliverInputEvents() {
        unsigned protonWidth,protonHeight;
        if(protonQueryXWireDesktop(protonWidth,protonHeight))return;''')
    c=once(c,'            memcpy(r + 8, &focus, 4);          // focus window','''            unsigned protonWidth,protonHeight;
                if(protonQueryXWireDesktop(protonWidth,protonHeight)){
                    const uint32_t actual=protonDesktopGetInputFocus();
                focus=actual; // None until a guest app owns keyboard focus; never canvas root.
                }
                memcpy(r + 8, &focus, 4);          // focus window''')
    c=once(c,'            uint32_t focusWin = rd32(req + 4);','''            uint32_t focusWin = rd32(req + 4);
                unsigned protonWidth,protonHeight;
                if(protonQueryXWireDesktop(protonWidth,protonHeight)){
                    protonDesktopSetInputFocus(focusWin);break;
                }''')
    c=once(c,'            uint16_t state = (uint16_t)(mod | buttonState);','''            unsigned protonWidth,protonHeight;
                if(protonQueryXWireDesktop(protonWidth,protonHeight)){
                    int localX=px,localY=py;uint32_t buttons=0;
                    if(protonDesktopPointerInfo(rd32(req+4),px,py,child,localX,localY,buttons)){
                        memcpy(r+12,&child,4);int16_t lx=(int16_t)localX,ly=(int16_t)localY;
                        memcpy(r+20,&lx,2);memcpy(r+22,&ly,2);mod|=buttons;
                    }
                }
                uint16_t state = (uint16_t)(mod | buttonState);''')
    c=once(c,'                if (dstWin) g_xwirePresentSink->warpPointer(dx, dy);','''                if(dstWin){
                        unsigned protonWidth,protonHeight;int ox=0,oy=0;
                        if(protonQueryXWireDesktop(protonWidth,protonHeight)&&protonDesktopRootOrigin(dstWin,ox,oy))
                            g_xwirePresentSink->warpPointer(dx+ox,dy+oy);
                        else g_xwirePresentSink->warpPointer(dx,dy);
                    }''')
    c += '\n' + DESKTOP_INPUT
    server=once(server,'#include <vector>','''#include <vector>
    extern bool protonQueryXWireDesktop(unsigned&,unsigned&);
    extern void protonPumpXWireDesktopInput(const std::vector<std::shared_ptr<XWireConnection>>&);
    ''')
    server=once(server,'    for (auto& c : snapshot) c->pumpInputAndFlush();','''    unsigned protonWidth,protonHeight;
        if(protonQueryXWireDesktop(protonWidth,protonHeight)){
            protonPumpXWireDesktopInput(snapshot);return;
        }
        for (auto& c : snapshot) c->pumpInputAndFlush();''')

    present=(s/'xwirepresentSDL.cpp').read_text()
    present=once(present,'#include "xwireconnection.h"','#include "xwireconnection.h"\nextern bool protonDesktopFocusPending();')
    present=once(present,'if (g_xwirePresentSink->hasInput()) {','if (g_xwirePresentSink->hasInput() || protonDesktopFocusPending()) {')
    outputs = {"xwireconnection.h": h, "xwireconnection.cpp": c,
               "xwireserver.cpp": server, "xwirepresentSDL.cpp": present}
    for name, value in outputs.items():
        (directory / name).write_text(value)
    return {"mode": "opt-in-root-coordinate-desktop-input",
            "source_files": ["source/x11wire/" + name for name in REQUIRED_TUS],
            "header_files": ["source/x11wire/" + name for name in REQUIRED_HEADERS],
            "sources": [str(directory / name) for name in REQUIRED_TUS],
            "rebuild_all_required_tus": True,
            "sha256": {name: hashlib.sha256(value.encode()).hexdigest()
                       for name, value in outputs.items()}}


# No registry/input-queue lock survives dispatch into a connection. Recipient
# shared_ptrs pin lifetime; the per-connection recursive mutex serializes protocol
# parsing, sequence numbers and output buffers against the sole SDL owner thread.
# Root-server window zero is never a focus/input recipient. No synthetic UI is used.
DESKTOP_INPUT = r'''

// Explicit desktop mode: frame origin is the X11 root, never the focused app.
namespace {
std::atomic<uint32_t> protonDesktopFocus{0},protonDesktopButtons{0},protonDesktopAnnouncedFocus{0};
uint32_t protonDesktopCapture=0,protonDesktopPointerWindow=0;
struct ProtonDesktopGeometry {
    int x=0,y=0,left=0,top=0,right=0,bottom=0;
    std::vector<uint64_t> order;
};
bool protonDesktopGeometryLocked(XWireServer& srv,uint32_t id,ProtonDesktopGeometry& out) {
    unsigned width,height;if(!protonQueryXWireDesktop(width,height))return false;
    out={};out.right=(int)width;out.bottom=(int)height;
    std::vector<const XWireWindow*> chain;
    uint32_t cursor=id;
    for(unsigned depth=0;depth<64;++depth) {
        auto it=srv.windows.find(cursor);
        if(it==srv.windows.end()||!it->second.mapped)return false;
        if(it->second.isRoot){cursor=0;break;}
        chain.push_back(&it->second);cursor=it->second.parent;
    }
    if(cursor)return false;
    for(auto it=chain.rbegin();it!=chain.rend();++it) {
        const auto& win=**it;out.x+=win.x;out.y+=win.y;
        out.left=std::max(out.left,out.x);out.top=std::max(out.top,out.y);
        out.right=std::min(out.right,out.x+(int)win.width);
        out.bottom=std::min(out.bottom,out.y+(int)win.height);
        out.order.push_back(win.mapSerial);
    }
    return out.right>out.left&&out.bottom>out.top;
}
uint32_t protonDesktopHitLocked(XWireServer& srv,int x,int y) {
    uint32_t best=0;ProtonDesktopGeometry bestGeometry;
    for(const auto& item:srv.windows) {
        if(item.second.isRoot)continue;
        ProtonDesktopGeometry geometry;
        if(!protonDesktopGeometryLocked(srv,item.first,geometry))continue;
        if(x<geometry.left||x>=geometry.right||y<geometry.top||y>=geometry.bottom)continue;
        if(!best||bestGeometry.order<geometry.order||
           (bestGeometry.order==geometry.order&&item.first>best)) {
            best=item.first;bestGeometry=std::move(geometry);
        }
    }
    return best;
}
uint32_t protonDesktopRecipientLocked(XWireServer& srv,uint32_t target,uint32_t mask) {
    for(unsigned depth=0;target&&depth<64;++depth) {
        auto it=srv.windows.find(target);
        if(it==srv.windows.end()||it->second.isRoot)return 0;
        if(it->second.eventMask&mask)return target;
        target=it->second.parent;
    }
    return 0;
}
std::shared_ptr<XWireConnection> protonDesktopOwner(
        const std::vector<std::shared_ptr<XWireConnection>>& clients,uint32_t target) {
    for(const auto& connection:clients)
        if(connection->ownsDesktopWindow(target))return connection;
    return {};
}
}
bool protonDesktopSetInputFocus(uint32_t window) {
    auto& srv=XWireServer::instance();std::lock_guard<std::mutex> lock(srv.regMutex);
    auto it=srv.windows.find(window);ProtonDesktopGeometry geometry;
    if(it==srv.windows.end()||it->second.isRoot||
       !protonDesktopGeometryLocked(srv,window,geometry))return false;
    protonDesktopFocus.store(window,std::memory_order_release);return true;
}
uint32_t protonDesktopGetInputFocus() {
    return protonDesktopFocus.load(std::memory_order_acquire);
}
bool protonDesktopPointerInfo(uint32_t query,int rootX,int rootY,uint32_t& child,
                             int& localX,int& localY,uint32_t& buttons) {
    auto& srv=XWireServer::instance();std::lock_guard<std::mutex> lock(srv.regMutex);
    ProtonDesktopGeometry geometry;
    if(!protonDesktopGeometryLocked(srv,query,geometry))return false;
    localX=rootX-geometry.x;localY=rootY-geometry.y;
    child=protonDesktopHitLocked(srv,rootX,rootY);
    for(unsigned depth=0;child&&depth<64;++depth) {
        auto it=srv.windows.find(child);
        if(child==query||it==srv.windows.end()){child=0;break;}
        if(it->second.parent==query)break;child=it->second.parent;
    }
    buttons=protonDesktopButtons.load(std::memory_order_acquire);return true;
}
bool protonDesktopRootOrigin(uint32_t window,int& x,int& y) {
    auto& srv=XWireServer::instance();std::lock_guard<std::mutex> lock(srv.regMutex);
    ProtonDesktopGeometry geometry;if(!protonDesktopGeometryLocked(srv,window,geometry))return false;
    x=geometry.x;y=geometry.y;return true;
}
bool XWireConnection::ownsDesktopWindow(uint32_t target) {
    std::lock_guard<std::recursive_mutex> lock(protonProtocolMutex);
    // A second client may still be completing its handshake on a guest thread.
    return clientIdBase&&(target&~clientIdMask)==clientIdBase;
}
void XWireConnection::deliverDesktopInput(uint32_t target,const XWireInputEvent& event,
                                         int originX,int originY,bool focus) {
    std::lock_guard<std::recursive_mutex> lock(protonProtocolMutex);
    // Neither registry nor another connection's protocol lock is held here.
    if((target&~clientIdMask)!=clientIdBase)return;
    uint8_t code=0;
    switch(event.type) {
        case XWireInputEvent::EvKeyDown:code=2;break;
        case XWireInputEvent::EvKeyUp:code=3;break;
        case XWireInputEvent::EvButtonDown:code=4;break;
        case XWireInputEvent::EvButtonUp:code=5;break;
        case XWireInputEvent::EvMotion:code=6;break;
    }
    if(!code)return;
    XWireInputEvent local=event;local.x-=originX;local.y-=originY;
    if(code>=4&&enteredWindow!=target) {
        sendCrossing(target,true,(int16_t)local.x,(int16_t)local.y);enteredWindow=target;
    }
    (void)focus; // Focus notifications are routed centrally to their actual client owners.
#ifdef PROTONDESKTOP_TRACE_XWIRE_INPUT
    klog_fmt("PROTON_DESKTOP_INPUT target=0x%x client=0x%x code=%u root=(%d,%d) local=(%d,%d)",
        target,clientIdBase,(unsigned)code,event.x,event.y,local.x,local.y);
#endif
    sendInputEvent(code,target,local,(int16_t)originX,(int16_t)originY);flushReplies();
}
void XWireConnection::leaveDesktopPointer(uint32_t target,int rootX,int rootY,
                                        int originX,int originY) {
    std::lock_guard<std::recursive_mutex> lock(protonProtocolMutex);
    if((target&~clientIdMask)!=clientIdBase)return;
    sendCrossing(target,false,(int16_t)(rootX-originX),(int16_t)(rootY-originY));
    if(enteredWindow==target)enteredWindow=0;flushReplies();
}
bool protonDesktopFocusPending() {
    unsigned width,height;
    return protonQueryXWireDesktop(width,height)&&
        protonDesktopFocus.load(std::memory_order_acquire)!=protonDesktopAnnouncedFocus.load(std::memory_order_acquire);
}
void XWireConnection::notifyDesktopFocus(uint32_t window,bool focused) {
    std::lock_guard<std::recursive_mutex> lock(protonProtocolMutex);
    if((window&~clientIdMask)!=clientIdBase)return;
    if(focused){sendFocusIn(window);lastFocus=window;}
    else{
        uint8_t event[32]={0};event[0]=10;event[1]=3;
        event[2]=(uint8_t)(sequence&0xff);event[3]=(uint8_t)(sequence>>8);
        std::memcpy(event+4,&window,4);writeToClient(event,sizeof(event));
        if(lastFocus==window)lastFocus=0;
    }
    flushReplies();
}
static void protonAnnounceDesktopFocus(const std::vector<std::shared_ptr<XWireConnection>>& clients) {
    uint32_t next=protonDesktopFocus.load(std::memory_order_acquire);
    {
        auto& srv=XWireServer::instance();std::lock_guard<std::mutex> lock(srv.regMutex);
        auto it=srv.windows.find(next);ProtonDesktopGeometry geometry;
        if(it==srv.windows.end()||it->second.isRoot||!protonDesktopGeometryLocked(srv,next,geometry)){
            if(next)protonDesktopFocus.compare_exchange_strong(next,0,std::memory_order_acq_rel);
            next=0;
        }
    }
    const uint32_t previous=protonDesktopAnnouncedFocus.load(std::memory_order_acquire);
    if(previous==next)return;
    auto oldOwner=protonDesktopOwner(clients,previous),newOwner=protonDesktopOwner(clients,next);
    if(previous&&oldOwner)oldOwner->notifyDesktopFocus(previous,false);
    if(next&&newOwner)newOwner->notifyDesktopFocus(next,true);
    protonDesktopAnnouncedFocus.store(next&&newOwner?next:0,std::memory_order_release);
}
void protonPumpXWireDesktopInput(const std::vector<std::shared_ptr<XWireConnection>>& clients) {
    if(!g_xwirePresentSink)return;
    // Called only by the sole SDL owner-thread pump; no queue/state lock survives dispatch.
    protonAnnounceDesktopFocus(clients);
    auto& srv=XWireServer::instance();XWireInputEvent event;
    while(g_xwirePresentSink->nextInputEvent(event)) {
        const bool key=event.type==XWireInputEvent::EvKeyDown||event.type==XWireInputEvent::EvKeyUp;
        const bool down=event.type==XWireInputEvent::EvButtonDown,up=event.type==XWireInputEvent::EvButtonUp;
        const uint32_t bit=(event.detail>=1&&event.detail<=24)?1u<<(7+event.detail):0;
        const uint32_t mask=key?(event.type==XWireInputEvent::EvKeyDown?1u:2u):(down?4u:(up?8u:0x40u));
        const uint32_t priorButtons=protonDesktopButtons.load(std::memory_order_acquire);
        uint32_t buttons=priorButtons;
        if(down)buttons|=bit;else if(up)buttons&=~bit;
        protonDesktopButtons.store(buttons,std::memory_order_release);
        event.state|=priorButtons;
        uint32_t target=0,oldPointer=0;ProtonDesktopGeometry geometry,oldGeometry;
        bool changeFocus=false;
        {
            std::lock_guard<std::mutex> lock(srv.regMutex);
            ProtonDesktopGeometry captureGeometry;
            if(protonDesktopCapture&&!protonDesktopGeometryLocked(srv,protonDesktopCapture,captureGeometry))
                protonDesktopCapture=0;
            target=key?protonDesktopFocus.load(std::memory_order_acquire):
                       (protonDesktopCapture?protonDesktopCapture:protonDesktopHitLocked(srv,event.x,event.y));
            if(up&&!buttons)protonDesktopCapture=0;
            target=protonDesktopRecipientLocked(srv,target,mask);
            if(!target||!protonDesktopGeometryLocked(srv,target,geometry))continue;
            if(!key&&target!=protonDesktopPointerWindow) {
                oldPointer=protonDesktopPointerWindow;
                if(!protonDesktopGeometryLocked(srv,oldPointer,oldGeometry))oldPointer=0;
                protonDesktopPointerWindow=target;
            }
            if(down) {
                changeFocus=protonDesktopFocus.exchange(target,std::memory_order_acq_rel)!=target;
                if(!protonDesktopCapture)protonDesktopCapture=target;
            }
        }
        auto owner=protonDesktopOwner(clients,target);
        if(!owner){klog_fmt("PROTON_DESKTOP_INPUT owner missing target=0x%x",target);continue;}
        if(oldPointer) {
            auto oldOwner=protonDesktopOwner(clients,oldPointer);
            if(oldOwner)oldOwner->leaveDesktopPointer(oldPointer,event.x,event.y,oldGeometry.x,oldGeometry.y);
        }
        if(changeFocus)protonAnnounceDesktopFocus(clients);
        owner->deliverDesktopInput(target,event,geometry.x,geometry.y,false);
    }
}

'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--overlay-root", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare_xwire_input(args.overlay_root, args.source_root), indent=2))


if __name__ == "__main__":
    main()
