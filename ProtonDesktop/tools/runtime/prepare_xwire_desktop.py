#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the opt-in real Wine desktop XWire source shadows.

The source checkout remains immutable. The legacy single-app profile is the
runtime default; the Wine host enables the scene before creating any X11 client.
Run this helper before prepare_xwire_input.py when input routing is enabled.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

PIN="58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4"

def prepare_xwire_desktop(source_root: Path, overlay_root: Path) -> dict:
    S=Path(source_root).resolve()
    output=Path(overlay_root).resolve()
    if output==S or output.is_relative_to(S):
        raise ValueError("The pristine source checkout cannot be the desktop overlay output")
    pin=subprocess.check_output(['git','-C',str(S),'rev-parse','HEAD'],text=True).strip()
    changed=subprocess.check_output(['git','-C',str(S),'status','--porcelain'],text=True)
    if pin!=PIN or changed:
        raise ValueError("The exact pristine pinned Boxedwine64 source checkout is required")
    D=output/'source/x11wire'
    def replace_n(s,a,b,count):
     if s.count(a)!=count:raise RuntimeError(('XWire desktop anchor mismatch',a[:100],s.count(a),count))
     return s.replace(a,b)
    def once(s,a,b):
     return replace_n(s,a,b,1)
    server=(S/'source/x11wire/xwireserver.cpp').read_text()
    extra=r'''
    #include <functional>
    #include <unordered_set>
    #include <stdexcept>
    namespace {
    std::atomic<unsigned> protonDesktopW{0}, protonDesktopH{0};
    std::mutex protonBackgroundMutex;
    std::unordered_map<uint32_t,uint32_t> protonBackgroundPixels;
    }
    void protonConfigureXWireDesktop(unsigned width,unsigned height) {
     if(width<640||width>3840||height<480||height>2160) throw std::runtime_error("Invalid Wine desktop size");
     protonDesktopH.store(height,std::memory_order_relaxed);protonDesktopW.store(width,std::memory_order_release);
    }
    bool protonQueryXWireDesktop(unsigned&width,unsigned&height) {
     width=protonDesktopW.load(std::memory_order_acquire);height=protonDesktopH.load(std::memory_order_relaxed);return width&&height;
    }
    void protonXWireSetBackground(uint32_t id,uint32_t pixel) {
     unsigned w,h;if(!protonQueryXWireDesktop(w,h))return;
     std::lock_guard<std::mutex>lock(protonBackgroundMutex);protonBackgroundPixels[id]=0xff000000u|(pixel&0xffffffu);
    }
    void protonXWireClearArea(uint32_t id,int x,int y,unsigned width,unsigned height) {
     unsigned dw,dh;if(!protonQueryXWireDesktop(dw,dh))return;
     XWireServer&srv=XWireServer::instance();
     {
     std::lock_guard<std::mutex>lock(srv.regMutex);auto it=srv.windows.find(id);if(it==srv.windows.end())return;auto&win=it->second;
     uint32_t pixel;
     {std::lock_guard<std::mutex>bg(protonBackgroundMutex);auto b=protonBackgroundPixels.find(id);if(b==protonBackgroundPixels.end())return;pixel=b->second;}
     unsigned ww=win.isRoot?dw:win.width,hh=win.isRoot?dh:win.height;if(!ww||!hh)return;
     if(win.fbW!=ww||win.fbH!=hh){std::vector<uint8_t>v((size_t)ww*hh*4,0);for(unsigned row=0;row<std::min<unsigned>(win.fbH,hh);++row)if(win.fb.size()>=(size_t)(row+1)*win.fbW*4)std::memcpy(v.data()+(size_t)row*ww*4,win.fb.data()+(size_t)row*win.fbW*4,(size_t)std::min<unsigned>(win.fbW,ww)*4);win.fb.swap(v);win.fbW=ww;win.fbH=hh;win.textFb.clear();}
     int left=std::max(0,x),top=std::max(0,y),right=std::min<int>(ww,width?x+(int)width:ww),bottom=std::min<int>(hh,height?y+(int)height:hh);
     for(int yy=top;yy<bottom;++yy)for(int xx=left;xx<right;++xx){((uint32_t*)win.fb.data())[(size_t)yy*ww+xx]=pixel;if(win.textFb.size()==win.fb.size())((uint32_t*)win.textFb.data())[(size_t)yy*ww+xx]=0;}
     }
     srv.schedulePresent();
    }
    static void protonComposeDesktop(XWireServer&srv,unsigned width,unsigned height) {
     struct Node{uint32_t id,parent;int x,y;unsigned w,h,fw,fh;bool mapped,root;uint64_t serial;std::vector<uint8_t>fb,text;};
     std::vector<Node>nodes;uint32_t root=0;
     {
     std::lock_guard<std::mutex>lock(srv.regMutex);
     for(const auto&kv:srv.windows){const auto&v=kv.second;nodes.push_back({kv.first,v.parent,v.x,v.y,v.width,v.height,v.fbW,v.fbH,v.mapped,v.isRoot,v.mapSerial,v.fb,v.textFb});if(v.isRoot)root=kv.first;}
     if(!root)return;srv.presentWindow=root;srv.glPresentDrawable=0;
     }
     std::unordered_map<uint32_t,const Node*>byId;std::unordered_map<uint32_t,std::vector<const Node*>>children;
     for(const auto&v:nodes){byId[v.id]=&v;if(v.id!=root)children[v.parent].push_back(&v);}
     for(auto&kv:children)std::sort(kv.second.begin(),kv.second.end(),[](const Node*a,const Node*b){return a->serial==b->serial?a->id<b->id:a->serial<b->serial;});
     // The canvas starts clear; every visible colored pixel below comes from a
     // guest framebuffer or a real X11 background-pixel request.
     std::vector<uint8_t>canvas((size_t)width*height*4,0);std::unordered_set<uint32_t>visited;
     std::function<void(const Node&,int,int,int,int,int,int,unsigned)>draw;
     draw=[&](const Node&v,int px,int py,int cl,int ct,int cr,int cb,unsigned depth){
      if(depth>64||!v.mapped||!visited.insert(v.id).second)return;
      int x=v.root?0:px+v.x,y=v.root?0:py+v.y;int right=v.root?(int)width:x+(int)v.w,bottom=v.root?(int)height:y+(int)v.h;
      int l=std::max(cl,x),t=std::max(ct,y),r=std::min(cr,right),b=std::min(cb,bottom);if(l>=r||t>=b)return;
      if(v.fw&&v.fh&&v.fb.size()>=(size_t)v.fw*v.fh*4){bool text=v.text.size()==v.fb.size();int rr=std::min(r,x+(int)v.fw),bb=std::min(b,y+(int)v.fh);for(int yy=t;yy<bb;++yy)for(int xx=l;xx<rr;++xx){size_t src=(size_t)(yy-y)*v.fw+(xx-x),dst=(size_t)yy*width+xx;((uint32_t*)canvas.data())[dst]=((const uint32_t*)v.fb.data())[src];if(text&&((const uint32_t*)v.text.data())[src])((uint32_t*)canvas.data())[dst]=((const uint32_t*)v.text.data())[src];}}
      auto ch=children.find(v.id);if(ch!=children.end())for(const Node*c:ch->second)draw(*c,x,y,l,t,r,b,depth+1);
     };
     auto rootNode=byId.find(root);if(rootNode==byId.end())return;draw(*rootNode->second,0,0,0,0,width,height,0);
     if(g_xwirePresentSink){g_xwirePresentSink->onWindowMapped(root,width,height);g_xwirePresentSink->submitFrame(root,width,height,canvas.data(),width*4);}
    }
    '''
    server=once(server,'XWireServer& XWireServer::instance() {',extra+'\nXWireServer& XWireServer::instance() {')
    server=once(server,'void XWireServer::composeAndPresent() {\n    if (!g_xwirePresentSink) return;', 'void XWireServer::composeAndPresent() {\n    if (!g_xwirePresentSink) return;\n    unsigned desktopW,desktopH;if(protonQueryXWireDesktop(desktopW,desktopH)){protonComposeDesktop(*this,desktopW,desktopH);return;}')
    conn=(S/'source/x11wire/xwireconnection.cpp').read_text();proto='''bool protonQueryXWireDesktop(unsigned&,unsigned&);
    void protonXWireSetBackground(uint32_t,uint32_t);
    void protonXWireClearArea(uint32_t,int,int,unsigned,unsigned);
    static bool protonDesktopMode(){unsigned w,h;return protonQueryXWireDesktop(w,h);}
    '''
    conn=once(conn,'XWireConnection::XWireConnection(',proto+'\nXWireConnection::XWireConnection(')
    conn=once(conn,'    // Constructed inside the guest\'s connect()', '    unsigned desktopW,desktopH;if(protonQueryXWireDesktop(desktopW,desktopH)){screenWidth=(uint16_t)desktopW;screenHeight=(uint16_t)desktopH;}\n    // Constructed inside the guest\'s connect()')
    # Prototypes must precede the value-list helper above the constructor.
    conn=conn.replace(proto+'\n','');idx=conn.index('namespace {');conn=conn[:idx]+proto+'\n'+conn[idx:]
    conn=once(conn,'void applyWindowValues(XWireWindow& w, uint32_t mask, const uint8_t* vals)', 'void applyWindowValues(XWireWindow& w, uint32_t mask, const uint8_t* vals, uint32_t windowId)')
    conn=once(conn,'                case CWOverrideRedirect:', '                case CWBackPixel: if(protonDesktopMode())protonXWireSetBackground(windowId,v);break;\n                case CWOverrideRedirect:')
    conn=once(conn,'applyWindowValues(w, rd32(req + 28), req + 32);','applyWindowValues(w, rd32(req + 28), req + 32, wid);')
    conn=once(conn,'applyWindowValues(it->second, mask, req + 12);','applyWindowValues(it->second, mask, req + 12, wid);')
    conn=once(conn,'                    it->second.parent = parent;','                    it->second.parent = parent;\n                    if(protonDesktopMode()){it->second.x=rx;it->second.y=ry;}')
    conn=once(conn,'            if (found) {\n                ensureWindow();','            if (found) {\n                if(protonDesktopMode())protonXWireClearArea(wid,0,0,0,0);\n                ensureWindow();')
    # Disable only legacy per-app canvas selection, while preserving all guest windows.
    conn=replace_n(conn,'if (srv.adoptNextWindowAsBase &&','if (!protonDesktopMode() && srv.adoptNextWindowAsBase &&',3)
    conn=replace_n(conn,'} else if (srv.presentWindow == 0) {','} else if (!protonDesktopMode() && srv.presentWindow == 0) {',2)
    conn=once(conn,'} else if (drawable != srv.presentWindow && !win.overrideRedirect && win.mapped) {','} else if (!protonDesktopMode() && drawable != srv.presentWindow && !win.overrideRedirect && win.mapped) {')
    conn=once(conn,'    if (win.isRoot) return;     // never present the root/desktop background','    if (win.isRoot && !protonDesktopMode()) return;     // legacy single-app profile skips root')
    conn=once(conn,'if (wit == srv.windows.end() || wit->second.isRoot) return; // dst not a window','if (wit == srv.windows.end() || (wit->second.isRoot && !protonDesktopMode())) return; // dst not a window')
    conn=replace_n(conn,'if (it == srv.windows.end() || it->second.isRoot) return;  // pixmap/root -> skip','if (it == srv.windows.end() || (it->second.isRoot && !protonDesktopMode())) return;  // legacy root skip',2)
    conn=once(conn,'if (it == srv.windows.end() || it->second.isRoot) return nullptr;','if (it == srv.windows.end() || (it->second.isRoot && !protonDesktopMode())) return nullptr;')
    # ClearArea has a different wire layout than PolyFillRectangle.
    conn=once(conn,'        case X_ClearArea:\n        case X_DeleteProperty:', '''        case X_ClearArea:
                if(protonDesktopMode()){
                    uint32_t wid=rd32(req+4);int16_t x=(int16_t)rd16(req+8),y=(int16_t)rd16(req+10);uint16_t w=rd16(req+12),h=rd16(req+14);
                    protonXWireClearArea(wid,x,y,w,h);if(req[1])sendExpose(wid,x,y,w?w:screenWidth,h?h:screenHeight);break;
                }
                [[fallthrough]];
            case X_DeleteProperty:''')
    # The Wine stage already removed synthetic startup painting and enforces
    # native SDL owner-thread presentation. Preserve that implementation.
    presenter_path=D/'xwirepresentSDL.cpp'
    if not presenter_path.is_file():
        raise ValueError("Generate the Wine-session presenter before the desktop overlay")
    present=presenter_path.read_text()
    if 'bool protonQueryXWireDesktop' in present:
        raise ValueError('The XWire desktop overlay is already applied')
    if 'protonWineGuestFrameSubmitted' not in present or 'protonWineCaptureFrame' in present:
        raise ValueError("Expected the production synthetic-disabled Wine presenter")
    present=once(present,'bool isMainthread();',
                 'bool isMainthread();\nbool protonQueryXWireDesktop(unsigned&,unsigned&);')
    before_filter="""        uint32_t base = srv.presentWindow;
        uint32_t gl = srv.glPresentDrawable;
        if (window && base && window != base && (!gl || window != gl)) return;"""
    submit_end=present.index('    void onWindowMapped(')
    submit=present[:submit_end]
    remainder=present[submit_end:]
    submit=once(submit,before_filter,"""        unsigned desktopW,desktopH;
        if (protonQueryXWireDesktop(desktopW,desktopH)) {
            // Only the composed real X11 root can replace a desktop canvas.
            std::lock_guard<std::mutex> registry(srv.regMutex);
            const auto root=srv.windows.find(window);
            if (root==srv.windows.end() || !root->second.isRoot ||
                width!=desktopW || height!=desktopH) return;
        } else {
"""+before_filter+"""
        }""")
    remainder=once(remainder,before_filter,"""        unsigned desktopW,desktopH;
        if (protonQueryXWireDesktop(desktopW,desktopH)) {
            // Mapping a child/menu never changes the host desktop extent.
            width=static_cast<uint16_t>(desktopW);
            height=static_cast<uint16_t>(desktopH);
        } else {
"""+before_filter+"""
        }""")
    present=submit+remainder
    # A static first frame must survive the finite swap-buffer clear series.
    # Reuse only genuine retained guest pixels, then become idle again. The
    # replay budget shares the pending-frame mutex with submit/event threads.
    present=once(present,'    bool haveFrame = false, frameDirty = false;',
                 '    bool haveFrame = false, frameDirty = false;\n'
                 '    unsigned protonReplayFrames = 0; // finite genuine-frame retries; guarded by mtx')
    present=once(present,'        if (pendingW != width || pendingH != height) {',
                 '        if (pendingW != width || pendingH != height) {\n'
                 '            unsigned desktopW,desktopH;\n'
                 '            if (protonQueryXWireDesktop(desktopW,desktopH)) protonReplayFrames=3;')
    present=once(present,'                shownW = wantW; shownH = wantH;',
                 '                shownW = wantW; shownH = wantH;\n'
                 '                unsigned desktopW,desktopH;\n'
                 '                if (protonQueryXWireDesktop(desktopW,desktopH)) protonReplayFrames=3;')
    present=once(present,'        frameDirty = true;     // ensure the tick does a present even if no new frame',
                 '        std::lock_guard<std::mutex> lock(mtx);\n'
                 '        frameDirty = true;\n'
                 '        unsigned desktopW,desktopH;\n'
                 '        if (protonQueryXWireDesktop(desktopW,desktopH)) protonReplayFrames=3;')
    present=once(present,'        uint16_t fw = 0, fh = 0;',
                 '        uint16_t fw = 0, fh = 0;\n        unsigned replayRemaining=0;')
    present=once(present,'            if (haveFrame && frameDirty) {',
                 '            if (haveFrame && (frameDirty || protonReplayFrames)) {')
    present=once(present,'                frameDirty = false;',
                 '                frameDirty = false;\n'
                 '                replayRemaining=protonReplayFrames;\n'
                 '                if (protonReplayFrames) --protonReplayFrames;')
    present=once(present,'            if (clearFrames > 0) {',
                 '            if (replayRemaining>static_cast<unsigned>(clearFrames))\n'
                 '                clearFrames=static_cast<int>(replayRemaining);\n'
                 '            if (clearFrames > 0) {')
    present=once(present,'        switch (e.type) {\n            case SDL_KEYDOWN:',
                 '''        switch (e.type) {
            case SDL_WINDOWEVENT: {
                unsigned desktopW,desktopH;
                if (protonQueryXWireDesktop(desktopW,desktopH) &&
                    (e.window.event==SDL_WINDOWEVENT_SHOWN ||
                     e.window.event==SDL_WINDOWEVENT_RESTORED ||
                     e.window.event==SDL_WINDOWEVENT_EXPOSED ||
                     e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED)) {
                    std::lock_guard<std::mutex> lock(mtx);
                    protonReplayFrames=3;
                }
                break;
            }
            case SDL_KEYDOWN:''')
    D.mkdir(parents=True,exist_ok=True)
    for name in ('xwireserver.h','xwireconnection.h','xwirepresent.h','xwirefont.h'):
        shutil.copyfile(S/'source/x11wire'/name,D/name)
    outputs={'xwireserver.cpp':server,'xwireconnection.cpp':conn,'xwirepresentSDL.cpp':present}
    for name,value in outputs.items():
        (D/name).write_text(value)
    return {'mode':'opt-in-real-wine-desktop','source_pin':PIN,
            'source_files':['source/x11wire/'+name for name in outputs],
            'header_files':['source/x11wire/'+name for name in ('xwireserver.h','xwireconnection.h','xwirepresent.h','xwirefont.h')],
            'sources':[str(D/name) for name in outputs],
            'sha256':{name:hashlib.sha256(value.encode()).hexdigest() for name,value in outputs.items()}}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--overlay-root',type=Path,required=True)
    args=parser.parse_args()
    print(json.dumps(prepare_xwire_desktop(args.source_root,args.overlay_root),indent=2))

if __name__=='__main__':
    main()
