#include "browser.h"

#include "magapi.h"
#include "remote_keymap.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QWebFrame>

#include <cstdio>

MagPage::MagPage(const QString &userAgent, QObject *parent)
    : QWebPage(parent)
    , agent(userAgent)
{
}

QString MagPage::userAgentForUrl(const QUrl &) const
{
    return agent;
}

bool MagPage::acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request, NavigationType type)
{
    Q_UNUSED(frame);
    Q_UNUSED(type);
    QUrl url = request.url();
    QString path = url.path();
    QString collapsed = path;
    while (collapsed.contains("/c/c"))
        collapsed.replace("/c/c", "/c");
    if (collapsed != path) {
        url.setPath(collapsed);
        fprintf(stderr, "rewrite %s\n", url.toString().toUtf8().constData());
        QTimer::singleShot(0, this, [this, url]() {
            mainFrame()->load(url);
        });
        return false;
    }
    fprintf(stderr, "nav %s\n", url.toString().toUtf8().constData());
    return true;
}

MagView::MagView(MagApi *api, QWidget *parent)
    : QWebView(parent)
    , api(api)
{
    setPage(new MagPage(api->userAgent(), this));
    page()->setPreferredContentsSize(QSize(1280, 720));
    page()->mainFrame()->setZoomFactor(1.5);
    page()->mainFrame()->setScrollBarPolicy(Qt::Horizontal, Qt::ScrollBarAlwaysOff);
    page()->mainFrame()->setScrollBarPolicy(Qt::Vertical, Qt::ScrollBarAlwaysOff);
    QPalette pal = palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Window, Qt::transparent);
    setPalette(pal);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    connect(page()->mainFrame(), SIGNAL(javaScriptWindowObjectCleared()), this, SLOT(attachObjects()));
    connect(this, SIGNAL(loadFinished(bool)), this, SLOT(onLoadFinished(bool)));
    connect(api, &MagApi::layoutChanged, this, [this]() { update(); });
    setFocusPolicy(Qt::StrongFocus);
    RemoteKeymap::instance().load();
    QTimer *holeTimer = new QTimer(this);
    connect(holeTimer, &QTimer::timeout, this, [this]() {
        QRect hole;
        if (this->api->previewHole(&hole))
            update(hole);
    });
    holeTimer->start(120);
}

void MagView::injectSettingsFullscreen()
{
    // Infomir settings is a floating 1142x584 window. Scale like the portal
    // (1280 logical @ 1.5x → 1920 physical) and stretch the UI to fill 1280x720.
    page()->mainFrame()->setZoomFactor(1.5);
    page()->mainFrame()->evaluateJavaScript(QStringLiteral(
        "(function(){"
        "try{"
        "window.stbWebWindow=window.stbWebWindow||{close:function(){try{stbWindowMgr.windowClose();}catch(e){}}};"
        "w=1280;h=720;a=0;b=0;put=720;"
        "var css=document.getElementById('yasem-settings-fill')||document.createElement('style');"
        "css.id='yasem-settings-fill';"
        "css.textContent="
        "'html,body{margin:0!important;padding:0!important;width:1280px!important;height:720px!important;"
        "overflow:hidden!important;background:#000!important;color:#fff;}"
        "#cont,iframe#cont{position:fixed!important;left:0!important;top:0!important;"
        "width:1280px!important;height:720px!important;border:0!important;visibility:visible!important;}"
        "#center{position:fixed!important;left:0!important;top:0!important;"
        "width:1280px!important;height:720px!important;background-size:100% 100%!important;}"
        ".header_text{margin-left:40px!important;}"
        ".modal,.modal_big{max-width:1100px!important;}'"
        ";"
        "if(document.documentElement)document.documentElement.appendChild(css);"
        "if(document.body){"
        "document.body.style.display='block';"
        "document.body.style.width='1280px';"
        "document.body.style.height='720px';"
        "document.body.style.margin='0';"
        "}"
        "var c=document.getElementById('cont');"
        "if(c){"
        "c.width=1280;c.height=720;"
        "c.style.position='fixed';"
        "c.style.left='0';c.style.top='0';"
        "c.style.width='1280px';c.style.height='720px';"
        "c.style.visibility='visible';"
        "try{cont=c;}catch(e){}"
        "}"
        // Hide unused Infomir settings entries (MAG HW / inner portal).
        "if(!window._yasemHideSettings){"
        "var hide={net_info:1,video:1,audio:1,net:1,advanced:1,internal_portal:1};"
        "function filterProf(){"
        "if(!window.prof||!window.prof.modules)return;"
        "var out=[],mods=window.prof.modules;"
        "for(var i=0;i<mods.length;i++){"
        "var m=mods[i];"
        "if(!m||hide[m.name])continue;"
        "out.push(m);"
        "}"
        "window.prof.modules=out;"
        "}"
        "function wrapInit(){"
        "var fn=window.init_m;"
        "if(typeof fn!=='function')return false;"
        "if(fn._yasemWrapped)return true;"
        "window.init_m=function(){filterProf();return fn.apply(this,arguments);};"
        "window.init_m._yasemWrapped=true;"
        "gSTB.Debug('settings hide wrap ok');"
        "if(window.prof&&window.prof.modules&&window.load_fl){"
        "try{filterProf();window.init_m();}catch(e){}"
        "}"
        "return true;"
        "}"
        "window._yasemHideSettings=true;"
        "if(!wrapInit()){"
        "var t=setInterval(function(){if(wrapInit())clearInterval(t);},50);"
        "setTimeout(function(){clearInterval(t);},10000);"
        "}"
        "}"
        "}catch(e){try{gSTB.Debug('settings_fill:'+e);}catch(e2){}}"
        "})();"
    ));
}

void MagView::attachObjects()
{
    QWebFrame *frame = page()->mainFrame();
    const QString href = frame->url().toString();
    const bool settingsPage = href.contains(QStringLiteral("external/settings"));
    // Same scale as portal: 1280 logical × 1.5 = 1920 physical (1080p/i panel).
    frame->setZoomFactor(1.5);
    page()->setPreferredContentsSize(QSize(1280, 720));
    frame->addToJavaScriptWindowObject(QStringLiteral("gSTB"), api);
    frame->addToJavaScriptWindowObject(QStringLiteral("stb"), api);
    frame->addToJavaScriptWindowObject(QStringLiteral("stbPlayer"), api);
    // Keep portal inject minimal. Never define stbWebWindow on the main portal —
    // Stalker treats that as "child web window" and skips normal UI.
    frame->evaluateJavaScript(QStringLiteral(
        "window.stbEvent=window.stbEvent||{event:0,onEvent:function(){}};"
        "window.stbEvent.bind=function(name,fn){if(name==='event'||name==='onEvent')this.onEvent=fn;};"
        "(function(){"
        "function openUrl(url){"
        "if(!url)return;"
        "try{var cur=String(window.location.href||'');"
        "if(cur&&cur.indexOf('external/settings')<0)gSTB.SetWebReturnUrl(cur);"
        "}catch(e){}"
        "try{gSTB.LoadURL(String(url));}catch(e){try{window.location.href=String(url);}catch(e2){}}"
        "}"
        "function closeUrl(){"
        "var u='';"
        "try{u=gSTB.GetWebReturnUrl()||'';gSTB.ClearWebReturnUrl();}catch(e){}"
        "if(!u){try{var e=JSON.parse(gSTB.GetEnv(JSON.stringify({varList:['portal1']})));u=e&&e.result&&e.result.portal1;}catch(e2){}}"
        "if(u){try{gSTB.LoadURL(String(u));}catch(e3){}}"
        "}"
        "window.stbWindowMgr={"
        "_yasem:true,"
        "openWebFavorites:function(url){openUrl(url);},"
        "openWebFace:function(url){openUrl(url);return 1;},"
        "openWindow:function(url){openUrl(url);return 1;},"
        "windowClose:function(){closeUrl();},"
        "closeWindow:function(){closeUrl();},"
        "CloseWebWindow:function(){closeUrl();},"
        "initWebWindow:function(url){if(url)openUrl(url);return 1;},"
        "LoadUrl:function(url){openUrl(url);},"
        "raiseWebWindow:function(){},"
        "raiseWebFaceWindow:function(){},"
        "setBookmarkImplUrl:function(){},"
        "IsWebWindowExist:function(){return false;},"
        "getCurrWebUrl:function(){try{return String(window.location.href||'');}catch(e){return '';}},"
        "getCurrentTitle:function(){try{return String(document.title||'');}catch(e){return '';}},"
        "hideWindowBackground:function(){},"
        "showBrowserBookmarks:function(){},"
        "setVirtualKeyboardInitAttr:function(){},"
        "setVirtualKeyboardImplUrl:function(){}"
        "};"
        "})();"
        "(function(){"
        "var w=1280,h=720;"
        "function lock(obj,name,value){try{Object.defineProperty(obj,name,{configurable:true,get:function(){return value;}});}catch(e){try{obj[name]=value;}catch(e2){}}}"
        "if(window.screen){lock(window.screen,'width',w);lock(window.screen,'height',h);lock(window.screen,'availWidth',w);lock(window.screen,'availHeight',h);}"
        "window.resizeTo=function(){};"
        "window.moveTo=function(){};"
        "var css=document.getElementById('yasem-clear')||document.createElement('style');"
        "css.id='yasem-clear';"
        "css.textContent='html,body{background:transparent !important;background-color:rgba(0,0,0,0) !important;}';"
        "if(document.documentElement) document.documentElement.appendChild(css);"
        "})();"
    ));
    if (settingsPage)
        injectSettingsFullscreen();
}

void MagView::onLoadFinished(bool ok)
{
    if (!ok)
        return;
    const QString href = page()->mainFrame()->url().toString();
    if (href.contains(QStringLiteral("external/settings"))) {
        injectSettingsFullscreen();
        // onLoad() sets floating size after scripts run — re-apply fullscreen.
        QTimer::singleShot(300, this, [this]() { injectSettingsFullscreen(); });
        QTimer::singleShot(1200, this, [this]() { injectSettingsFullscreen(); });
        QTimer::singleShot(3000, this, [this]() { injectSettingsFullscreen(); });
        page()->mainFrame()->evaluateJavaScript(QStringLiteral(
            "(function(){"
            "try{"
            "if(!window._yasemSettingsHook){"
            "window._yasemSettingsHook=true;"
            "window.onerror=function(m,s,l){try{gSTB.Debug('settings_js:'+m+' @'+s+':'+l);}catch(e){}}"
            "}"
            "}catch(e){}"
            "})();"
        ));
        return;
    }
    QTimer::singleShot(2500, this, [this]() { injectClockPersist(); });
    QTimer::singleShot(7000, this, [this]() { injectClockPersist(); });
}

void MagView::injectClockPersist()
{
    // Only watch+save. Do not call changeType from C++ inject (broke clock UI).
    // User enables clock from MAG menu or CLOCK key; we persist for next boot restore
    // done inside portal after a short delay via stb.user + Refresh only.
    page()->mainFrame()->evaluateJavaScript(QStringLiteral(
        "(function(){"
        "try{"
        "if(!window.stb||!stb.player||!stb.player.ClockOnVideo)return;"
        "var C=stb.player.ClockOnVideo;"
        "if(!C.dom_obj)return;"
        "if(!window._yasemClockWatch){"
        "window._yasemClockWatch=true;"
        "var last=(stb.user&&stb.user.video_clock)||null;"
        "setInterval(function(){"
        "try{"
        "var v=(stb.user&&stb.user.video_clock)||'Off';"
        "if(v===last)return;"
        "last=v;"
        "gSTB.SaveUserData('video_clock.json',JSON.stringify({video_clock:String(v)}));"
        "}catch(e){}"
        "},1500);"
        "}"
        "if(C._yasemClockRestored)return;"
        "C._yasemClockRestored=true;"
        "var raw=gSTB.LoadUserData('video_clock.json')||'';"
        "if(!raw)return;"
        "var data=JSON.parse(raw);"
        "if(!data||!data.video_clock||data.video_clock==='Off')return;"
        "stb.user=stb.user||{};"
        "stb.user.video_clock=data.video_clock;"
        "C.position=data.video_clock;"
        "C.on=true;"
        "try{C.dom_obj.className='osd_clock_block onVideo'+data.video_clock;C.dom_obj.style.display='block';C.dom_obj.show&&C.dom_obj.show();}catch(e){}"
        "try{stb.clock&&stb.clock.show&&stb.clock.show();}catch(e){}"
        "}catch(e){}"
        "})();"
    ));
    fprintf(stderr, "injectClockPersist watch+soft-restore\n");
}

void MagView::paintEvent(QPaintEvent *event)
{
    QWebView::paintEvent(event);
    QRect hole;
    if (!api->previewHole(&hole))
        return;
    QPainter painter(this);
    painter.setClipping(false);
    painter.setClipRect(hole, Qt::ReplaceClip);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.fillRect(hole, Qt::transparent);
}

static bool tooSoon(int native)
{
    static int lastNative = -1;
    static QElapsedTimer timer;
    if (!timer.isValid())
        timer.start();
    qint64 elapsed = timer.elapsed();
    // IR remotes often emit a second press within ~300ms (looks like double Down/RED).
    if (native == lastNative && elapsed < 350)
        return true;
    lastNative = native;
    timer.restart();
    return false;
}

// MAG254 / Stalker portal keyCodes (JS). QtWebKit often mishandles these.
static int magDomKeyCode(const QKeyEvent &ev)
{
    switch (ev.key()) {
    case Qt::Key_Backspace: return 8;     // key.BACK
    case Qt::Key_Escape: return 27;       // key.EXIT
    case Qt::Key_Home: return 122;        // treat as MENU if raw Home arrives
    case Qt::Key_F1: return 112;
    case Qt::Key_F2: return 113;
    case Qt::Key_F3: return 114;
    case Qt::Key_F4: return 115;
    case Qt::Key_F8: return 119;          // key.EPG
    case Qt::Key_F10: return 121;         // key.TV
    case Qt::Key_F11: return 122;         // key.MENU (Home button)
    case Qt::Key_F12: return 123;         // key.APP
    case Qt::Key_Y: return 89;            // key.INFO base (needs alt → 2089)
    case Qt::Key_Space: return 32;        // key.CLOCK base (needs alt → 2032)
    case Qt::Key_VolumeMute: return 192;  // key.MUTE base (needs alt → 2192)
    default: return -1;
    }
}

// Stalker: document keydown → keydown_observer.fire().
// Prefer a SINGLE emulate_key() call. Never dispatch on both window+document
// (that fires the portal handler twice — e.g. RED looks like two Downs).
static void fireMagDomKey(QWebFrame *frame, int keyCode, bool alt = false, bool ctrl = false,
    bool settingsPage = false)
{
    if (!frame || keyCode < 0)
        return;
    int stalkerCode = keyCode + (alt ? 2000 : 0) + (ctrl ? 1000 : 0);
    // Infomir settings: EXIT closes (or back()); MENU is unused — treat as EXIT.
    if (settingsPage && stalkerCode == 122)
        stalkerCode = 27;
    if (settingsPage) {
        // Key handlers live in iframe#cont (glavnaya / g_*.html), not the parent.
        frame->evaluateJavaScript(QStringLiteral(
            "(function(code){"
            "function synth(c){return {keyCode:c,which:c,preventDefault:function(){}}}"
            "var iframe=document.getElementById('cont');"
            "var win=iframe&&iframe.contentWindow;"
            "if(win&&typeof win.pressKey==='function'){"
            "try{win.pressKey(synth(code));return;}catch(e){}"
            "}"
            "if(typeof pressKey==='function'){"
            "try{pressKey(synth(code));return;}catch(e){}"
            "}"
            "if(code===27||code===8){"
            "try{stbWebWindow.close();}catch(e){try{stbWindowMgr.windowClose();}catch(e2){}}"
            "}"
            "})(%1);").arg(stalkerCode));
        return;
    }
    frame->evaluateJavaScript(QStringLiteral(
        "(function(code){"
        "if(typeof keydown_observer!=='undefined'&&keydown_observer.emulate_key){"
        "keydown_observer.emulate_key(code);return;}"
        "var e=document.createEvent('Events');e.initEvent('keydown',true,true);"
        "try{Object.defineProperty(e,'keyCode',{get:function(){return code}});}catch(x){e.keyCode=code;}"
        "try{Object.defineProperty(e,'which',{get:function(){return code}});}catch(x){e.which=code;}"
        "document.dispatchEvent(e);"
        "})(%1);").arg(stalkerCode));
}

void MagView::keyPressEvent(QKeyEvent *event)
{
    if (RemoteKeymap::instance().isPower(event)) {
        if (event->isAutoRepeat() || tooSoon(event->nativeScanCode()))
            return;
        // After wake, ignore Power briefly — same IR press must not re-enter standby.
        if (QFile::exists(QStringLiteral("/storage/yasem/.power-guard"))) {
            fprintf(stderr, "power key ignored (wake guard)\n");
            return;
        }
        // Enter soft standby: blank + stop Yasem. Watcher waits for Power to start Yasem again.
        fprintf(stderr, "power key -> standby (stop yasem)\n");
        api->StandBy(1);
        QProcess::startDetached(QStringLiteral("/bin/systemctl"),
            QStringList() << QStringLiteral("start") << QStringLiteral("yasem-standby.service"));
        return;
    }
    if (event->isAutoRepeat() || tooSoon(event->nativeScanCode()))
        return;
    fprintf(stderr, "raw native=%d linux=%d qtkey=%d\n",
        event->nativeScanCode(), event->nativeScanCode() >= 8 ? event->nativeScanCode() - 8 : event->nativeScanCode(),
        event->key());
    QKeyEvent translated = RemoteKeymap::instance().translate(event);
    const int linuxCode = event->nativeScanCode() >= 8 ? event->nativeScanCode() - 8 : event->nativeScanCode();
    fprintf(stderr, "key %d from linux %d\n", translated.key(), linuxCode);

    const int dom = magDomKeyCode(translated);
    if (dom >= 0) {
        const bool alt = (translated.modifiers() & Qt::AltModifier)
            || translated.key() == Qt::Key_Y
            || translated.key() == Qt::Key_Space
            || translated.key() == Qt::Key_VolumeMute;
        const bool ctrl = (translated.modifiers() & Qt::ControlModifier);
        const bool settingsPage = page()->mainFrame()->url().toString()
            .contains(QStringLiteral("external/settings"));
        fprintf(stderr, "mag dom keyCode %d alt=%d settings=%d\n",
            dom, alt ? 1 : 0, settingsPage ? 1 : 0);
        fireMagDomKey(page()->mainFrame(), dom, alt, ctrl, settingsPage);
        // Color/special keys: JS only. Do NOT forward to WebKit —
        // RED is linux KEY_TAB(15); Tab in the page moves focus (looks like Down).
        if (translated.key() == Qt::Key_VolumeMute
            || translated.key() == Qt::Key_Escape
            || translated.key() == Qt::Key_Home
            || translated.key() == Qt::Key_Backspace
            || translated.key() == Qt::Key_F1
            || translated.key() == Qt::Key_F2
            || translated.key() == Qt::Key_F3
            || translated.key() == Qt::Key_F4
            || translated.key() == Qt::Key_F8
            || translated.key() == Qt::Key_F10
            || translated.key() == Qt::Key_F11
            || translated.key() == Qt::Key_F12
            || translated.key() == Qt::Key_Y
            || translated.key() == Qt::Key_Space)
            return;
    }
    // Never let raw Tab reach the portal (focus traversal ≈ Down).
    if (translated.key() == Qt::Key_Tab || translated.key() == Qt::Key_Backtab)
        return;
    QWebView::keyPressEvent(&translated);
}

void MagView::keyReleaseEvent(QKeyEvent *event)
{
    if (RemoteKeymap::instance().isPower(event) || api->isStandBy())
        return;
    QKeyEvent translated = RemoteKeymap::instance().translate(event);
    QWebView::keyReleaseEvent(&translated);
}
