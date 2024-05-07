#include "waveform/waveformwidgetfactory.h"

#ifdef MIXXX_USE_QOPENGL
#include <QOpenGLShaderProgram>
#include <QOpenGLWindow>
#else
#include <QGLFormat>
#include <QGLShaderProgram>
#endif

#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QStringList>
#include <QWidget>
#include <QWindow>

#include "moc_waveformwidgetfactory.cpp"
#include "util/cmdlineargs.h"
#include "util/math.h"
#include "util/performancetimer.h"
#include "util/timer.h"
#include "waveform/guitick.h"
#include "waveform/sharedglcontext.h"
#include "waveform/visualsmanager.h"
#include "waveform/vsyncthread.h"
#ifdef MIXXX_USE_QOPENGL
#include "waveform/widgets/allshader/filteredwaveformwidget.h"
#include "waveform/widgets/allshader/hsvwaveformwidget.h"
#include "waveform/widgets/allshader/lrrgbwaveformwidget.h"
#include "waveform/widgets/allshader/rgbwaveformwidget.h"
#include "waveform/widgets/allshader/simplewaveformwidget.h"
#else
#include "waveform/widgets/qthsvwaveformwidget.h"
#include "waveform/widgets/qtrgbwaveformwidget.h"
#include "waveform/widgets/qtsimplewaveformwidget.h"
#include "waveform/widgets/qtvsynctestwidget.h"
#include "waveform/widgets/qtwaveformwidget.h"
#endif
#include "waveform/widgets/emptywaveformwidget.h"
#include "waveform/widgets/glrgbwaveformwidget.h"
#include "waveform/widgets/glsimplewaveformwidget.h"
#include "waveform/widgets/glslwaveformwidget.h"
#include "waveform/widgets/glvsynctestwidget.h"
#include "waveform/widgets/glwaveformwidget.h"
#include "waveform/widgets/hsvwaveformwidget.h"
#include "waveform/widgets/rgbwaveformwidget.h"
#include "waveform/widgets/softwarewaveformwidget.h"
#include "waveform/widgets/waveformwidgetabstract.h"
#include "widget/wvumeterbase.h"
#include "widget/wvumeterlegacy.h"
#include "widget/wwaveformviewer.h"

namespace {
// Returns true if the given waveform should be rendered.
bool shouldRenderWaveform(WaveformWidgetAbstract* pWaveformWidget) {
    if (pWaveformWidget == nullptr ||
        pWaveformWidget->getWidth() == 0 ||
        pWaveformWidget->getHeight() == 0) {
        return false;
    }

    auto* glw = pWaveformWidget->getGLWidget();
    if (glw == nullptr) {
        // Not a WGLWidget. We can simply use QWidget::isVisible.
        auto* qwidget = qobject_cast<QWidget*>(pWaveformWidget->getWidget());
        return qwidget != nullptr && qwidget->isVisible();
    }

    return glw->shouldRender();
}

const QRegularExpression openGLVersionRegex(QStringLiteral("^(\\d+)\\.(\\d+).*$"));
}  // anonymous namespace

///////////////////////////////////////////

WaveformWidgetAbstractHandle::WaveformWidgetAbstractHandle()
        : m_type(WaveformWidgetType::Count_WaveformWidgetType) {
}

///////////////////////////////////////////

WaveformWidgetHolder::WaveformWidgetHolder()
        : m_waveformWidget(nullptr),
          m_waveformViewer(nullptr),
          m_skinContextCache(UserSettingsPointer(), QString()) {
}

WaveformWidgetHolder::WaveformWidgetHolder(WaveformWidgetAbstract* waveformWidget,
                                           WWaveformViewer* waveformViewer,
                                           const QDomNode& node,
                                           const SkinContext& parentContext)
    : m_waveformWidget(waveformWidget),
      m_waveformViewer(waveformViewer),
      m_skinNodeCache(node.cloneNode()),
      m_skinContextCache(&parentContext) {
}

///////////////////////////////////////////

WaveformWidgetFactory::WaveformWidgetFactory()
        // Set an empty waveform initially. We will set the correct one when skin load finishes.
        // Concretely, we want to set a non-GL waveform when loading the skin so that the window
        // loads correctly.
        : m_type(WaveformWidgetType::Empty),
          m_configType(WaveformWidgetType::Empty),
          m_config(nullptr),
          m_skipRender(false),
          m_frameRate(60),
          m_endOfTrackWarningTime(30),
          m_defaultZoom(WaveformWidgetRenderer::s_waveformDefaultZoom),
          m_zoomSync(true),
          m_overviewNormalized(false),
          m_untilMarkShowBeats(false),
          m_untilMarkShowTime(false),
          m_untilMarkAlign(Qt::AlignVCenter),
          m_untilMarkTextPointSize(24),
          m_openGlAvailable(false),
          m_openGlesAvailable(false),
          m_openGLShaderAvailable(false),
          m_beatGridAlpha(90),
          m_vsyncThread(nullptr),
          m_pGuiTick(nullptr),
          m_pVisualsManager(nullptr),
          m_frameCnt(0),
          m_actualFrameRate(0),
          m_playMarkerPosition(WaveformWidgetRenderer::s_defaultPlayMarkerPosition) {
    m_visualGain[All] = 1.0;
    m_visualGain[Low] = 1.0;
    m_visualGain[Mid] = 1.0;
    m_visualGain[High] = 1.0;

#ifdef MIXXX_USE_QOPENGL
    WGLWidget* widget = SharedGLContext::getWidget();
    if (widget) {
        widget->makeCurrentIfNeeded();
        auto* pContext = QOpenGLContext::currentContext();
        if (pContext) {
            auto* glFunctions = pContext->functions();
            glFunctions->initializeOpenGLFunctions();
            QString versionString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_VERSION))));
            QString vendorString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_VENDOR))));
            QString rendererString = QString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_RENDERER))));
            qDebug().noquote() << QStringLiteral(
                    "OpenGL driver version string \"%1\", vendor \"%2\", "
                    "renderer \"%3\"")
                                          .arg(versionString, vendorString, rendererString);

            GLint majorVersion, minorVersion = GL_INVALID_ENUM;
            glFunctions->glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
            glFunctions->glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
            if (majorVersion == GL_INVALID_ENUM || minorVersion == GL_INVALID_ENUM) {
                // GL_MAJOR/MINOR_VERSION are not supported below OpenGL 3.0, so
                // parse GL_VERSION string as a fallback.
                // https://www.khronos.org/opengl/wiki/OpenGL_Context#OpenGL_version_number
                auto match = openGLVersionRegex.match(versionString);
                DEBUG_ASSERT(match.hasMatch());
                majorVersion = match.captured(1).toInt();
                minorVersion = match.captured(2).toInt();
            }

            qDebug().noquote()
                    << QStringLiteral("Supported OpenGL version: %1.%2")
                               .arg(QString::number(majorVersion), QString::number(minorVersion));

            m_openGLShaderAvailable = QOpenGLShaderProgram::hasOpenGLShaderPrograms(pContext);

            m_openGLVersion = pContext->isOpenGLES() ? "ES " : "";
            m_openGLVersion += majorVersion == 0 ? QString("None") : versionString;

            // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
            if (pContext->isOpenGLES()) {
                if (majorVersion * 100 + minorVersion >= 200) {
                    m_openGlesAvailable = true;
                }
            } else {
                if (majorVersion * 100 + minorVersion >= 201) {
                    m_openGlAvailable = true;
                }
            }

            if (!rendererString.isEmpty()) {
                m_openGLVersion += " (" + rendererString + ")";
            }
        } else {
            qDebug() << "QOpenGLContext::currentContext() returns nullptr";
        }
        widget->doneCurrent();
        widget->hide();
    }
#else
    QGLWidget* pGlWidget = SharedGLContext::getWidget();
    if (pGlWidget && pGlWidget->isValid()) {
        // will be false if SafeMode is enabled

        pGlWidget->show();
        // Without a makeCurrent, hasOpenGLShaderPrograms returns false on Qt 5.
        // and QGLFormat::openGLVersionFlags() returns the maximum known version
        pGlWidget->makeCurrent();

        QGLFormat::OpenGLVersionFlags version = QGLFormat::openGLVersionFlags();

        auto rendererString = QString();
        if (QOpenGLContext::currentContext()) {
            auto glFunctions = QOpenGLFunctions();

            glFunctions.initializeOpenGLFunctions();
            QString versionString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_VERSION))));
            QString vendorString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_VENDOR))));
            rendererString = QString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_RENDERER))));

            // Either GL or GL ES Version is set, not both.
            qDebug() << QString("openGLVersionFlags 0x%1").arg(version, 0, 16) << versionString << vendorString << rendererString;
        } else {
            qDebug() << "QOpenGLContext::currentContext() returns nullptr";
            qDebug() << "pGlWidget->->windowHandle() =" << pGlWidget->windowHandle();
        }

        int majorGlVersion = 0;
        int minorGlVersion = 0;
        int majorGlesVersion = 0;
        int minorGlesVersion = 0;
        if (version == QGLFormat::OpenGL_Version_None) {
            m_openGLVersion = "None";
        } else if (version & QGLFormat::OpenGL_Version_4_3) {
            majorGlVersion = 4;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_4_2) {
            majorGlVersion = 4;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_4_1) {
            majorGlVersion = 4;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_4_0) {
            majorGlVersion = 4;
            minorGlVersion = 0;
        } else if (version & QGLFormat::OpenGL_Version_3_3) {
            majorGlVersion = 3;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_3_2) {
            majorGlVersion = 3;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_3_1) {
            majorGlVersion = 3;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_3_0) {
            majorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_2_1) {
            majorGlVersion = 2;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_2_0) {
            majorGlVersion = 2;
            minorGlVersion = 0;
        } else if (version & QGLFormat::OpenGL_Version_1_5) {
            majorGlVersion = 1;
            minorGlVersion = 5;
        } else if (version & QGLFormat::OpenGL_Version_1_4) {
            majorGlVersion = 1;
            minorGlVersion = 4;
        } else if (version & QGLFormat::OpenGL_Version_1_3) {
            majorGlVersion = 1;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_1_2) {
            majorGlVersion = 1;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_1_1) {
            majorGlVersion = 1;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_Version_2_0) {
            m_openGLVersion = "ES 2.0";
            majorGlesVersion = 2;
            minorGlesVersion = 0;
        } else if (version & QGLFormat::OpenGL_ES_CommonLite_Version_1_1) {
            if (version & QGLFormat::OpenGL_ES_Common_Version_1_1) {
                m_openGLVersion = "ES 1.1";
            } else {
                m_openGLVersion = "ES Common Lite 1.1";
            }
            majorGlesVersion = 1;
            minorGlesVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_Common_Version_1_1) {
            m_openGLVersion = "ES Common Lite 1.1";
            majorGlesVersion = 1;
            minorGlesVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_CommonLite_Version_1_0) {
            if (version & QGLFormat::OpenGL_ES_Common_Version_1_0) {
                m_openGLVersion = "ES 1.0";
            } else {
                m_openGLVersion = "ES Common Lite 1.0";
            }
            majorGlesVersion = 1;
            minorGlesVersion = 0;
        } else if (version & QGLFormat::OpenGL_ES_Common_Version_1_0) {
            m_openGLVersion = "ES Common Lite 1.0";
            majorGlesVersion = 1;
            minorGlesVersion = 0;
        } else {
            m_openGLVersion = QString("Unknown 0x%1")
                .arg(version, 0, 16);
        }

        if (majorGlVersion != 0) {
            m_openGLVersion = QString::number(majorGlVersion) + "."
                    + QString::number(minorGlVersion);

#if !defined(QT_NO_OPENGL) && !defined(QT_OPENGL_ES_2)
            if (majorGlVersion * 100 + minorGlVersion >= 201) {
                // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                m_openGlAvailable = true;
            }
#endif
        } else {
            if (majorGlesVersion * 100 + minorGlesVersion >= 200) {
                // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                m_openGlesAvailable = true;
            }
        }

        m_openGLShaderAvailable =
                QGLShaderProgram::hasOpenGLShaderPrograms(
                        pGlWidget->context());

        if (!rendererString.isEmpty()) {
            m_openGLVersion += " (" + rendererString + ")";
        }

        pGlWidget->hide();
    }
#endif
    evaluateWidgets();
    m_time.start();
}

WaveformWidgetFactory::~WaveformWidgetFactory() {
    if (m_vsyncThread) {
        delete m_vsyncThread;
    }
}

bool WaveformWidgetFactory::setConfig(UserSettingsPointer config) {
    m_config = config;
    if (!m_config) {
        return false;
    }

    bool ok = false;

    int frameRate = m_config->getValue(ConfigKey("[Waveform]","FrameRate"), m_frameRate);
    m_frameRate = math_clamp(frameRate, 1, 120);


    int endTime = m_config->getValueString(ConfigKey("[Waveform]","EndOfTrackWarningTime")).toInt(&ok);
    if (ok) {
        setEndOfTrackWarningTime(endTime);
    } else {
        m_config->set(ConfigKey("[Waveform]","EndOfTrackWarningTime"),
                ConfigValue(m_endOfTrackWarningTime));
    }

    double defaultZoom = m_config->getValueString(ConfigKey("[Waveform]","DefaultZoom")).toDouble(&ok);
    if (ok) {
        setDefaultZoom(defaultZoom);
    } else{
        m_config->set(ConfigKey("[Waveform]","DefaultZoom"), ConfigValue(m_defaultZoom));
    }

    bool zoomSync = m_config->getValue(ConfigKey("[Waveform]", "ZoomSynchronization"), m_zoomSync);
    setZoomSync(zoomSync);

    int beatGridAlpha = m_config->getValue(ConfigKey("[Waveform]", "beatGridAlpha"), m_beatGridAlpha);
    setDisplayBeatGridAlpha(beatGridAlpha);

    WaveformWidgetType::Type type = static_cast<WaveformWidgetType::Type>(
            m_config->getValueString(ConfigKey("[Waveform]","WaveformType")).toInt(&ok));
    // Store the widget type on m_configType for later initialization.
    // We will initialize the objects later because of a problem with GL on QT 5.14.2 on Windows
    if (!ok || !setWidgetType(type, &m_configType)) {
        setWidgetType(WaveformWidgetType::RGB, &m_configType);
    }

    for (int i = 0; i < FilterCount; i++) {
        double visualGain = m_config->getValueString(
                ConfigKey("[Waveform]","VisualGain_" + QString::number(i))).toDouble(&ok);

        if (ok) {
            setVisualGain(FilterIndex(i), visualGain);
        } else {
            m_config->set(ConfigKey("[Waveform]","VisualGain_" + QString::number(i)),
                          QString::number(m_visualGain[i]));
        }
    }

    int overviewNormalized = m_config->getValueString(ConfigKey("[Waveform]","OverviewNormalized")).toInt(&ok);
    if (ok) {
        setOverviewNormalized(static_cast<bool>(overviewNormalized));
    } else {
        m_config->set(ConfigKey("[Waveform]","OverviewNormalized"), ConfigValue(m_overviewNormalized));
    }

    m_playMarkerPosition = m_config->getValue(ConfigKey("[Waveform]","PlayMarkerPosition"),
            WaveformWidgetRenderer::s_defaultPlayMarkerPosition);
    setPlayMarkerPosition(m_playMarkerPosition);

    int untilMarkShowBeats =
            m_config->getValueString(
                            ConfigKey("[Waveform]", "UntilMarkShowBeats"))
                    .toInt(&ok);
    if (ok) {
        setUntilMarkShowBeats(static_cast<bool>(untilMarkShowBeats));
    } else {
        m_config->set(ConfigKey("[Waveform]", "UntilMarkShowBeats"),
                ConfigValue(m_untilMarkShowBeats));
    }
    int untilMarkShowTime =
            m_config->getValueString(
                            ConfigKey("[Waveform]", "UntilMarkShowTime"))
                    .toInt(&ok);
    if (ok) {
        setUntilMarkShowTime(static_cast<bool>(untilMarkShowTime));
    } else {
        m_config->set(ConfigKey("[Waveform]", "UntilMarkShowTime"),
                ConfigValue(m_untilMarkShowTime));
    }

    setUntilMarkAlign(toUntilMarkAlign(
            m_config->getValue(ConfigKey("[Waveform]", "UntilMarkAlign"),
                    toUntilMarkAlignIndex(m_untilMarkAlign))));
    setUntilMarkTextPointSize(
            m_config->getValue(ConfigKey("[Waveform]", "UntilMarkTextPointSize"),
                    m_untilMarkTextPointSize));

    return true;
}

void WaveformWidgetFactory::destroyWidgets() {
    for (auto& holder : m_waveformWidgetHolders) {
        WaveformWidgetAbstract* pWidget = holder.m_waveformWidget;
        holder.m_waveformWidget = nullptr;
        delete pWidget;
    }
    m_waveformWidgetHolders.clear();
}

void WaveformWidgetFactory::addVuMeter(WVuMeterLegacy* pVuMeter) {
    // Do not hold the pointer to of timer listeners since they may be deleted.
    // We don't activate update() or repaint() directly so listener widgets
    // can decide whether to paint or not.
    connect(this,
            &WaveformWidgetFactory::waveformUpdateTick,
            pVuMeter,
            &WVuMeterLegacy::maybeUpdate,
            Qt::DirectConnection);
}

void WaveformWidgetFactory::addVuMeter(WVuMeterBase* pVuMeter) {
    // WVuMeterGLs to be rendered and swapped from the vsync thread
    connect(this,
            &WaveformWidgetFactory::renderVuMeters,
            pVuMeter,
            &WVuMeterBase::render);
    connect(this,
            &WaveformWidgetFactory::swapVuMeters,
            pVuMeter,
            &WVuMeterBase::swap);
}

void WaveformWidgetFactory::slotSkinLoaded() {
    setWidgetTypeFromConfig();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && defined __WINDOWS__
    // This regenerates the waveforms twice because of a bug found on Windows
    // where the first one fails.
    // The problem is that the window of the widget thinks that it is not exposed.
    // (https://doc.qt.io/qt-5/qwindow.html#exposeEvent )
    setWidgetTypeFromConfig();
#endif
}

bool WaveformWidgetFactory::setWaveformWidget(WWaveformViewer* viewer,
                                              const QDomElement& node,
                                              const SkinContext& parentContext) {
    int index = findIndexOf(viewer);
    if (index != -1) {
        qDebug() << "WaveformWidgetFactory::setWaveformWidget - "\
                    "viewer already have a waveform widget but it's not found by the factory !";
        delete viewer->getWaveformWidget();
    }

    // Cast to widget done just after creation because it can't be perform in
    // constructor (pure virtual)
    WaveformWidgetAbstract* waveformWidget = createWaveformWidget(m_type, viewer);
    viewer->setWaveformWidget(waveformWidget);
    viewer->setup(node, parentContext);

    // create new holder
    WaveformWidgetHolder holder(waveformWidget, viewer, node, &parentContext);
    if (index == -1) {
        // add holder
        m_waveformWidgetHolders.push_back(std::move(holder));
        index = static_cast<int>(m_waveformWidgetHolders.size()) - 1;
    } else {
        // update holder
        DEBUG_ASSERT(index >= 0);
        m_waveformWidgetHolders[index] = std::move(holder);
    }

    viewer->setZoom(m_defaultZoom);
    viewer->setDisplayBeatGridAlpha(m_beatGridAlpha);
    viewer->setPlayMarkerPosition(m_playMarkerPosition);
    waveformWidget->resize(viewer->width(), viewer->height());
    waveformWidget->getWidget()->show();
    viewer->update();

    qDebug() << "WaveformWidgetFactory::setWaveformWidget - waveform widget added in factory, index" << index;

    return true;
}

void WaveformWidgetFactory::setFrameRate(int frameRate) {
    m_frameRate = math_clamp(frameRate, 1, 120);
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]","FrameRate"), ConfigValue(m_frameRate));
    }
    if (m_vsyncThread) {
        m_vsyncThread->setSyncIntervalTimeMicros(static_cast<int>(1e6 / m_frameRate));
    }
}

void WaveformWidgetFactory::setEndOfTrackWarningTime(int endTime) {
    m_endOfTrackWarningTime = endTime;
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]","EndOfTrackWarningTime"), ConfigValue(m_endOfTrackWarningTime));
    }
}

bool WaveformWidgetFactory::setWidgetType(WaveformWidgetType::Type type) {
    return setWidgetType(type, &m_type);
}

bool WaveformWidgetFactory::setWidgetType(
        WaveformWidgetType::Type type,
        WaveformWidgetType::Type* pCurrentType) {
    if (type == *pCurrentType) {
        return true;
    }

    // check if type is acceptable
    int index = findHandleIndexFromType(type);
    bool isAcceptable = index > -1;
    *pCurrentType = isAcceptable ? type : WaveformWidgetType::Empty;
    if (m_config) {
        m_configType = *pCurrentType;
        m_config->setValue(
                ConfigKey("[Waveform]", "WaveformType"), *pCurrentType);
    }
    return isAcceptable;
}

bool WaveformWidgetFactory::widgetTypeSupportsUntilMark() const {
    switch (m_configType) {
    case WaveformWidgetType::RGB:
        return true;
    case WaveformWidgetType::Filtered:
        return true;
    case WaveformWidgetType::Simple:
        return true;
    case WaveformWidgetType::HSV:
        return true;
    default:
        break;
    }
    return false;
}

bool WaveformWidgetFactory::setWidgetTypeFromConfig() {
    int empty = findHandleIndexFromType(WaveformWidgetType::Empty);
    int desired = findHandleIndexFromType(m_configType);
    if (desired == -1) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromConfig"
                 << " - configured type" << static_cast<int>(m_configType)
                 << "not found -- using 'EmptyWaveform'";
        desired = empty;
    }
    return setWidgetTypeFromHandle(desired, true);
}

bool WaveformWidgetFactory::setWidgetTypeFromHandle(int handleIndex, bool force) {
    if (handleIndex < 0 || handleIndex >= m_waveformWidgetHandles.size()) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromHandle"
                    " - invalid handle --> using 'EmptyWaveform'";
        // fallback empty type
        setWidgetType(WaveformWidgetType::Empty);
        return false;
    }

    WaveformWidgetAbstractHandle& handle = m_waveformWidgetHandles[handleIndex];
    if (handle.m_type == m_type && !force) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromHandle - type"
                 << handle.getDisplayName() << "already in use";
        return true;
    }

    // change the type
    setWidgetType(handle.m_type);

    m_skipRender = true;

    //re-create/setup all waveform widgets
    for (auto& holder : m_waveformWidgetHolders) {
        WaveformWidgetAbstract* previousWidget = holder.m_waveformWidget;
        TrackPointer pTrack = previousWidget->getTrackInfo();
        //previousWidget->hold();
        double previousZoom = previousWidget->getZoomFactor();
        double previousPlayMarkerPosition = previousWidget->getPlayMarkerPosition();
        int previousbeatgridAlpha = previousWidget->getBeatGridAlpha();
        delete previousWidget;
        WWaveformViewer* viewer = holder.m_waveformViewer;
        WaveformWidgetAbstract* widget = createWaveformWidget(m_type, holder.m_waveformViewer);
        holder.m_waveformWidget = widget;
        viewer->setWaveformWidget(widget);
        viewer->setup(holder.m_skinNodeCache, holder.m_skinContextCache);
        viewer->setZoom(previousZoom);
        viewer->setPlayMarkerPosition(previousPlayMarkerPosition);
        viewer->setDisplayBeatGridAlpha(previousbeatgridAlpha);
        // resize() doesn't seem to get called on the widget. I think Qt skips
        // it since the size didn't change.
        //viewer->resize(viewer->size());
        widget->resize(viewer->width(), viewer->height());
        widget->setTrack(pTrack);
        widget->getWidget()->show();
        viewer->update();
    }

    m_skipRender = false;
    return true;
}

void WaveformWidgetFactory::setDefaultZoom(double zoom) {
    m_defaultZoom = math_clamp(zoom, WaveformWidgetRenderer::s_waveformMinZoom,
                               WaveformWidgetRenderer::s_waveformMaxZoom);
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]","DefaultZoom"), ConfigValue(m_defaultZoom));
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformViewer->setZoom(m_defaultZoom);
    }
}

void WaveformWidgetFactory::setZoomSync(bool sync) {
    m_zoomSync = sync;
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]","ZoomSynchronization"), ConfigValue(m_zoomSync));
    }

    if (m_waveformWidgetHolders.size() == 0) {
        return;
    }

    double refZoom = m_waveformWidgetHolders[0].m_waveformWidget->getZoomFactor();
    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformViewer->setZoom(refZoom);
    }
}

void WaveformWidgetFactory::setDisplayBeatGridAlpha(int alpha) {
    m_beatGridAlpha = alpha;
    if (m_waveformWidgetHolders.size() == 0) {
        return;
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformWidget->setDisplayBeatGridAlpha(m_beatGridAlpha);
    }
}

void WaveformWidgetFactory::setVisualGain(FilterIndex index, double gain) {
    m_visualGain[index] = gain;
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]","VisualGain_" + QString::number(index)), QString::number(m_visualGain[index]));
    }
}

double WaveformWidgetFactory::getVisualGain(FilterIndex index) const {
    return m_visualGain[index];
}

void WaveformWidgetFactory::setOverviewNormalized(bool normalize) {
    m_overviewNormalized = normalize;
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]","OverviewNormalized"), ConfigValue(m_overviewNormalized));
    }
}

void WaveformWidgetFactory::setPlayMarkerPosition(double position) {
    m_playMarkerPosition = position;
    if (m_config) {
        m_config->setValue(ConfigKey("[Waveform]", "PlayMarkerPosition"), m_playMarkerPosition);
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformWidget->setPlayMarkerPosition(m_playMarkerPosition);
    }
}

void WaveformWidgetFactory::notifyZoomChange(WWaveformViewer* viewer) {
    WaveformWidgetAbstract* pWaveformWidget = viewer->getWaveformWidget();
    if (pWaveformWidget == nullptr || !isZoomSync()) {
        return;
    }
    double refZoom = pWaveformWidget->getZoomFactor();

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        if (holder.m_waveformViewer != viewer) {
            holder.m_waveformViewer->setZoom(refZoom);
        }
    }
}

void WaveformWidgetFactory::renderSelf() {
    ScopedTimer t(u"WaveformWidgetFactory::render() %1waveforms",
            static_cast<int>(m_waveformWidgetHolders.size()));

    if (!m_skipRender) {
        if (m_type) {   // no regular updates for an empty waveform
            // next rendered frame is displayed after next buffer swap and than after VSync
            QVarLengthArray<bool, 10> shouldRenderWaveforms(
                    static_cast<int>(m_waveformWidgetHolders.size()));
            for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                    i < m_waveformWidgetHolders.size();
                    i++) {
                WaveformWidgetAbstract* pWaveformWidget = m_waveformWidgetHolders[i].m_waveformWidget;
                // Don't bother doing the pre-render work if we aren't going to
                // render this widget.
                bool shouldRender = shouldRenderWaveform(pWaveformWidget);
                shouldRenderWaveforms[static_cast<int>(i)] = shouldRender;
                if (!shouldRender) {
                    continue;
                }
                // Calculate play position for the new Frame in following run
                pWaveformWidget->preRender(m_vsyncThread);
            }
            //qDebug() << "prerender" << m_vsyncThread->elapsed();

            // It may happen that there is an artificially delayed due to
            // anti tearing driver settings
            // all render commands are delayed until the swap from the previous run is executed
            for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                    i < m_waveformWidgetHolders.size();
                    i++) {
                WaveformWidgetAbstract* pWaveformWidget = m_waveformWidgetHolders[i].m_waveformWidget;
                if (!shouldRenderWaveforms[static_cast<int>(i)]) {
                    continue;
                }
                pWaveformWidget->render();
                //qDebug() << "render" << i << m_vsyncThread->elapsed();
            }
        }

        // WSpinnys are also double-buffered WGLWidgets, like all the waveform
        // renderers. Render all the WSpinny widgets now.
        emit renderSpinnies(m_vsyncThread);
        // Same for WVuMeterGL. Note that we are either using WVuMeter or WVuMeterGL.
        // If we are using WVuMeter, this does nothing
        emit renderVuMeters(m_vsyncThread);

        // Notify all other waveform-like widgets (e.g. WSpinny's) that they should
        // update.
        //int t1 = m_vsyncThread->elapsed();
        emit waveformUpdateTick();
        //qDebug() << "emit" << m_vsyncThread->elapsed() - t1;

        m_frameCnt += 1.0f;
        mixxx::Duration timeCnt = m_time.elapsed();
        if (timeCnt > mixxx::Duration::fromSeconds(1)) {
            m_time.start();
            m_frameCnt = m_frameCnt * 1000 / timeCnt.toIntegerMillis(); // latency correction
            emit waveformMeasured(m_frameCnt, m_vsyncThread->droppedFrames());
            m_frameCnt = 0.0;
        }
    }

    m_pVisualsManager->process(m_endOfTrackWarningTime);
    m_pGuiTick->process();

    //qDebug() << "refresh end" << m_vsyncThread->elapsed();
}

void WaveformWidgetFactory::render() {
    renderSelf();
    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::swapSelf() {
    ScopedTimer t(u"WaveformWidgetFactory::swap() %1waveforms",
            static_cast<int>(m_waveformWidgetHolders.size()));

    // Do this in an extra slot to be sure to hit the desired interval
    if (!m_skipRender) {
        if (m_type) {   // no regular updates for an empty waveform
            // Show rendered buffer from last render() run
            //qDebug() << "swap() start" << m_vsyncThread->elapsed();
            for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
                WaveformWidgetAbstract* pWaveformWidget = holder.m_waveformWidget;

                // Don't swap invalid / invisible widgets or widgets with an
                // unexposed window. Prevents continuous log spew of
                // "QOpenGLContext::swapBuffers() called with non-exposed
                // window, behavior is undefined" on Qt5. See issue #9360.
                if (!shouldRenderWaveform(pWaveformWidget)) {
                    continue;
                }
                WGLWidget* glw = pWaveformWidget->getGLWidget();
                if (glw != nullptr) {
                    glw->makeCurrentIfNeeded();
                    glw->swapBuffers();
                    glw->doneCurrent();
                }
                //qDebug() << "swap x" << m_vsyncThread->elapsed();
            }
        }
        // WSpinnys are also double-buffered QGLWidgets, like all the waveform
        // renderers. Swap all the WSpinny widgets now.
        emit swapSpinnies();
        // Same for WVuMeterGL. Note that we are either using WVuMeter or WVuMeterGL
        // If we are using WVuMeter, this does nothing
        emit swapVuMeters();
    }
}

void WaveformWidgetFactory::swap() {
    swapSelf();
    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::swapAndRender() {
    // used for PLL
    WGLWidget* widget = SharedGLContext::getWidget();
    widget->getOpenGLWindow()->update();

    swapSelf();
    renderSelf();

    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::slotFrameSwapped() {
#ifdef MIXXX_USE_QOPENGL
    if (m_vsyncThread->pllInitializing()) {
        // continuously trigger redraws during PLL init
        WGLWidget* widget = SharedGLContext::getWidget();
        widget->getOpenGLWindow()->update();
    }
    // update the phase-locked-loop
    m_vsyncThread->updatePLL();
#endif
}

void WaveformWidgetFactory::evaluateWidgets() {
    m_waveformWidgetHandles.clear();
    QHash<WaveformWidgetType::Type, QList<WaveformWidgetBackend::Backend>> collectedHandles;
    for (int type = WaveformWidgetType::Empty;
            type < WaveformWidgetType::Count_WaveformWidgetType;
            type++) {
        // this lambda needs its type specified explicitly,
        // requiring it to be called with via `.operator()<WaveformT>()`
        collectedHandles.insert(static_cast<WaveformWidgetType::Type>(type),
                QList<WaveformWidgetBackend::Backend>());
        auto setWaveformVarsByType = [&]<typename WaveformT>() {
            bool useOpenGl = WaveformT::useOpenGl();
            bool useOpenGles = WaveformT::useOpenGles();
            bool useOpenGLShaders = WaveformT::useOpenGLShaders();
            WaveformWidgetCategory category = WaveformT::category();
            WaveformWidgetBackend::Backend backend = WaveformWidgetBackend::None;

            bool active = true;
            if (isOpenGlAvailable()) {
                if (useOpenGles && !useOpenGl) {
                    active = false;
                } else if (useOpenGLShaders && !isOpenGlShaderAvailable()) {
                    active = false;
                }
            } else if (isOpenGlesAvailable()) {
                if (useOpenGl && !useOpenGles) {
                    active = false;
                } else if (useOpenGLShaders && !isOpenGlShaderAvailable()) {
                    active = false;
                }
            } else {
                // No sufficient GL support
                if (useOpenGles || useOpenGl || useOpenGLShaders) {
                    active = false;
                }
            }

            if (category == WaveformWidgetCategory::DeveloperOnly &&
                    !CmdlineArgs::Instance().getDeveloper()) {
                active = false;
            }
#ifdef MIXXX_USE_QOPENGL
            else if (category == WaveformWidgetCategory::AllShader) {
                backend = WaveformWidgetBackend::AllShader;
            }
#endif
            else if (category == WaveformWidgetCategory::Legacy && useOpenGLShaders) {
                backend = WaveformWidgetBackend::GLSL;
            } else if (category == WaveformWidgetCategory::Legacy) {
                backend = WaveformWidgetBackend::GL;
            }

            if (active) {
                collectedHandles[static_cast<WaveformWidgetType::Type>(type)].push_back(backend);
            }
        };

        switch(type) {
        case WaveformWidgetType::Empty:
            setWaveformVarsByType.operator()<EmptyWaveformWidget>();
            break;
        case WaveformWidgetType::Simple:
            setWaveformVarsByType.operator()<GLSimpleWaveformWidget>();
#ifdef MIXXX_USE_QOPENGL
            setWaveformVarsByType.operator()<allshader::SimpleWaveformWidget>();
#else
            setWaveformVarsByType.operator()<QtSimpleWaveformWidget>();
            break;
#endif
        case WaveformWidgetType::Filtered:
#ifndef __APPLE__
            // Don't offer the simple renderers on macOS, they do not work with skins
            // that load GL widgets (spinnies, waveforms) in singletons.
            // Also excluded in enum WaveformWidgetType
            // https://bugs.launchpad.net/bugs/1928772
            setWaveformVarsByType.operator()<SoftwareWaveformWidget>();
#endif
            setWaveformVarsByType.operator()<GLWaveformWidget>();
            setWaveformVarsByType.operator()<GLSLFilteredWaveformWidget>();
#ifdef MIXXX_USE_QOPENGL
            setWaveformVarsByType.operator()<allshader::FilteredWaveformWidget>();
#else
            setWaveformVarsByType.operator()<QtWaveformWidget>();
#endif
            break;
        case WaveformWidgetType::VSyncTest:
            setWaveformVarsByType.operator()<GLVSyncTestWidget>();
#ifndef MIXXX_USE_QOPENGL
            setWaveformVarsByType.operator()<QtVSyncTestWidget>();
#endif
            break;
        case WaveformWidgetType::RGB:
            setWaveformVarsByType.operator()<GLSLRGBWaveformWidget>();
            setWaveformVarsByType.operator()<GLSLRGBStackedWaveformWidget>();
            setWaveformVarsByType.operator()<GLRGBWaveformWidget>();
#ifndef __APPLE__
            // Don't offer the simple renderers on macOS, they do not work with skins
            // that load GL widgets (spinnies, waveforms) in singletons.
            // Also excluded in enum WaveformWidgetType
            // https://bugs.launchpad.net/bugs/1928772
            setWaveformVarsByType.operator()<RGBWaveformWidget>();
#endif
#ifdef MIXXX_USE_QOPENGL
            setWaveformVarsByType.operator()<allshader::RGBWaveformWidget>();
#else
            setWaveformVarsByType.operator()<QtRGBWaveformWidget>();
#endif
            break;
        case WaveformWidgetType::HSV:
#ifndef __APPLE__
            // Don't offer the simple renderers on macOS, they do not work with skins
            // that load GL widgets (spinnies, waveforms) in singletons.
            // Also excluded in enum WaveformWidgetType
            // https://bugs.launchpad.net/bugs/1928772
            setWaveformVarsByType.operator()<HSVWaveformWidget>();
#endif
#ifdef MIXXX_USE_QOPENGL
            setWaveformVarsByType.operator()<allshader::HSVWaveformWidget>();
#else
            setWaveformVarsByType.operator()<QtHSVWaveformWidget>();
#endif
        case WaveformWidgetType::Stacked:
            setWaveformVarsByType.operator()<GLSLRGBStackedWaveformWidget>();
            break;
        default:
            DEBUG_ASSERT(!"Unexpected WaveformWidgetType");
            continue;
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    for (auto [type, backends] : collectedHandles.asKeyValueRange()) {
#else
    QHashIterator<WaveformWidgetType::Type,
            QList<WaveformWidgetBackend::Backend>>
            handleIter(collectedHandles);
    while (handleIter.hasNext()) {
        handleIter.next();
        auto& type = handleIter.key();
        auto& backends = handleIter.value();
#endif
        m_waveformWidgetHandles.push_back(WaveformWidgetAbstractHandle(type, backends));
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createFilteredWaveformWidget(
        WWaveformViewer* viewer) {
    // On the UI, hardware acceleration is a boolean (0 => software rendering, 1
    // => hardware acceleration), but in the setting, we keep the granularity so
    // in case of issue when we release, we can communicate workaround on
    // editing the INI file to target a specific rendering backend. If no
    // complains come back, we can convert this safely to a backend eventually.
    int backend = m_config->getValue(
            ConfigKey("[Waveform]", "use_hardware_acceleration"),
            isOpenGlAvailable() || isOpenGlesAvailable()
                    ?
#ifdef MIXXX_USE_QOPENGL
                    WaveformWidgetBackend::AllShader
#else
                    WaveformWidgetBackend::GL
#endif
                    : WaveformWidgetBackend::None);

    switch (backend) {
    case WaveformWidgetBackend::GL:
        return new GLWaveformWidget(viewer->getGroup(), viewer);
    case WaveformWidgetBackend::GLSL:
        return new GLSLFilteredWaveformWidget(viewer->getGroup(), viewer);
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return new allshader::FilteredWaveformWidget(viewer->getGroup(), viewer);
#else
    case WaveformWidgetBackend::Qt:
        return new QtWaveformWidget(viewer->getGroup(), viewer);
#endif
    default:
        return new SoftwareWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createHSVWaveformWidget(WWaveformViewer* viewer) {
    // On the UI, hardware acceleration is a boolean (0 => software rendering, 1
    // => hardware acceleration), but in the setting, we keep the granularity so
    // in case of issue when we release, we can communicate workaround on
    // editing the INI file to target a specific rendering backend. If no
    // complains come back, we can convert this safely to a backend eventually.
    int backend = m_config->getValue(
            ConfigKey("[Waveform]", "use_hardware_acceleration"),
            isOpenGlAvailable() || isOpenGlesAvailable()
                    ?
#ifdef MIXXX_USE_QOPENGL
                    WaveformWidgetBackend::AllShader
#else
                    WaveformWidgetBackend::GL
#endif
                    : WaveformWidgetBackend::None);

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return new allshader::HSVWaveformWidget(viewer->getGroup(), viewer);
#endif
    default:
        return new HSVWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createRGBWaveformWidget(WWaveformViewer* viewer) {
    // On the UI, hardware acceleration is a boolean (0 => software rendering, 1
    // => hardware acceleration), but in the setting, we keep the granularity so
    // in case of issue when we release, we can communicate workaround on
    // editing the INI file to target a specific rendering backend. If no
    // complains come back, we can convert this safely to a backend eventually.
    int backend = m_config->getValue(
            ConfigKey("[Waveform]", "use_hardware_acceleration"),
            isOpenGlAvailable() || isOpenGlesAvailable()
                    ?
#ifdef MIXXX_USE_QOPENGL
                    WaveformWidgetBackend::AllShader
#else
                    WaveformWidgetBackend::GL
#endif
                    : WaveformWidgetBackend::None);

    switch (backend) {
    case WaveformWidgetBackend::GL:
        return new GLRGBWaveformWidget(viewer->getGroup(), viewer);
    case WaveformWidgetBackend::GLSL:
        return new GLSLRGBWaveformWidget(viewer->getGroup(), viewer);
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return new allshader::RGBWaveformWidget(viewer->getGroup(), viewer);
#else
    case WaveformWidgetBackend::Qt:
        return new QtRGBWaveformWidget(viewer->getGroup(), viewer);
#endif
    default:
        return new RGBWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createStackedWaveformWidget(
        WWaveformViewer* viewer) {
    return new GLSLRGBStackedWaveformWidget(viewer->getGroup(), viewer);
}

WaveformWidgetAbstract* WaveformWidgetFactory::createSimpleWaveformWidget(WWaveformViewer* viewer) {
    // On the UI, hardware acceleration is a boolean (0 => software rendering, 1
    // => hardware acceleration), but in the setting, we keep the granularity so
    // in case of issue when we release, we can communicate workaround on
    // editing the INI file to target a specific rendering backend. If no
    // complains come back, we can convert this safely to a backend eventually.
    int backend = m_config->getValue(
            ConfigKey("[Waveform]", "use_hardware_acceleration"),
            isOpenGlAvailable() || isOpenGlesAvailable()
                    ?
#ifdef MIXXX_USE_QOPENGL
                    WaveformWidgetBackend::AllShader
#else
                    WaveformWidgetBackend::GL
#endif
                    : WaveformWidgetBackend::None);

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::GL:
        return new GLSimpleWaveformWidget(viewer->getGroup(), viewer);
    default:
        return new allshader::SimpleWaveformWidget(viewer->getGroup(), viewer);
#else
    case WaveformWidgetBackend::Qt:
        return new QtSimpleWaveformWidget(viewer->getGroup(), viewer);
    default:
        return new GLSimpleWaveformWidget(viewer->getGroup(), viewer);
#endif
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createVSyncTestWaveformWidget(
        WWaveformViewer* viewer) {
#ifdef MIXXX_USE_QOPENGL
    return new GLVSyncTestWidget(viewer->getGroup(), viewer);
#else
    return new QtVSyncTest(viewer->getGroup(), viewer);
#endif
}

WaveformWidgetAbstract* WaveformWidgetFactory::createWaveformWidget(
        WaveformWidgetType::Type type, WWaveformViewer* viewer) {
    WaveformWidgetAbstract* widget = nullptr;
    if (viewer) {
        if (CmdlineArgs::Instance().getSafeMode()) {
            type = WaveformWidgetType::Empty;
        }

        switch(type) {
        case WaveformWidgetType::Simple:
            widget = createSimpleWaveformWidget(viewer);
            break;
        case WaveformWidgetType::Filtered:
            widget = createFilteredWaveformWidget(viewer);
            break;
        case WaveformWidgetType::HSV:
            widget = createHSVWaveformWidget(viewer);
            break;
        case WaveformWidgetType::VSyncTest:
            widget = createVSyncTestWaveformWidget(viewer);
            break;
        case WaveformWidgetType::RGB:
            widget = createRGBWaveformWidget(viewer);
            break;
        case WaveformWidgetType::Stacked:
            widget = createStackedWaveformWidget(viewer);
            break;
        //case WaveformWidgetType::EmptyWaveform:
        default:
            widget = new EmptyWaveformWidget(viewer->getGroup(), viewer);
            break;
        }
        widget->castToQWidget();
        if (!widget->isValid()) {
            qWarning() << "failed to init WaveformWidget" << type << "fall back to \"Empty\"";
            delete widget;
            widget = new EmptyWaveformWidget(viewer->getGroup(), viewer);
            widget->castToQWidget();
            if (!widget->isValid()) {
                qWarning() << "failed to init EmptyWaveformWidget";
                delete widget;
                widget = nullptr;
            }
        }
    }
    return widget;
}

int WaveformWidgetFactory::findIndexOf(WWaveformViewer* viewer) const {
    for (int i = 0; i < (int)m_waveformWidgetHolders.size(); i++) {
        if (m_waveformWidgetHolders[i].m_waveformViewer == viewer) {
            return i;
        }
    }
    return -1;
}

void WaveformWidgetFactory::startVSync(GuiTick* pGuiTick, VisualsManager* pVisualsManager) {
    const auto vSyncMode = static_cast<VSyncThread::VSyncMode>(
            m_config->getValue(ConfigKey("[Waveform]", "VSync"), 0));

    m_pGuiTick = pGuiTick;
    m_pVisualsManager = pVisualsManager;
    m_vsyncThread = new VSyncThread(this, vSyncMode);
    m_vsyncThread->setObjectName(QStringLiteral("VSync"));
    m_vsyncThread->setSyncIntervalTimeMicros(static_cast<int>(1e6 / m_frameRate));

#ifdef MIXXX_USE_QOPENGL
    if (m_vsyncThread->vsyncMode() == VSyncThread::ST_PLL) {
        WGLWidget* widget = SharedGLContext::getWidget();
        connect(widget->getOpenGLWindow(),
                &QOpenGLWindow::frameSwapped,
                this,
                &WaveformWidgetFactory::slotFrameSwapped,
                Qt::DirectConnection);
        widget->show();
    }
#endif

    connect(m_vsyncThread,
            &VSyncThread::vsyncRender,
            this,
            &WaveformWidgetFactory::render);
    connect(m_vsyncThread,
            &VSyncThread::vsyncSwap,
            this,
            &WaveformWidgetFactory::swap);
    connect(m_vsyncThread,
            &VSyncThread::vsyncSwapAndRender,
            this,
            &WaveformWidgetFactory::swapAndRender);

    m_vsyncThread->start(QThread::NormalPriority);
}

void WaveformWidgetFactory::getAvailableVSyncTypes(QList<QPair<int, QString>>* pList) {
    m_vsyncThread->getAvailableVSyncTypes(pList);
}

WaveformWidgetType::Type WaveformWidgetFactory::findTypeFromHandleIndex(int index) {
    WaveformWidgetType::Type type = WaveformWidgetType::Count_WaveformWidgetType;
    if (index >= 0 && index < m_waveformWidgetHandles.size()) {
        type = m_waveformWidgetHandles[index].m_type;
    }
    return type;
}

int WaveformWidgetFactory::findHandleIndexFromType(WaveformWidgetType::Type type) {
    int index = -1;
    for (int i = 0; i < m_waveformWidgetHandles.size(); i++) {
        const WaveformWidgetAbstractHandle& handle = m_waveformWidgetHandles[i];
        if (handle.m_type == type) {
            index = i;
        }
    }
    return index;
}

// Static
QString WaveformWidgetAbstractHandle::getDisplayName() const {
    switch (m_type) {
    case WaveformWidgetType::Empty:
        return QObject::tr("Empty");
    case WaveformWidgetType::Simple:
        return QObject::tr("Simple");
    case WaveformWidgetType::Filtered:
        return QObject::tr("Filtered");
    case WaveformWidgetType::HSV:
        return QObject::tr("HSV");
    case WaveformWidgetType::VSyncTest:
        return QObject::tr("VSyncTest");
    case WaveformWidgetType::RGB:
        return QObject::tr("RGB");
    case WaveformWidgetType::Stacked:
        return QObject::tr("Stacked");
    default:
        return QObject::tr("Unknown");
    }
}

// static
QSurfaceFormat WaveformWidgetFactory::getSurfaceFormat(UserSettingsPointer config) {
    // The first call should pass the config to set the vsync mode. Subsequent
    // calls will use the value as set on the first call.
    static const auto vsyncMode = config->getValue(ConfigKey("[Waveform]", "VSync"), 0);

    QSurfaceFormat format;
    // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0, default is 2.0
    // format.setVersion(2, 1);
    // Core and Compatibility contexts have been introduced in openGL 3.2
    // From 3.0 to 3.1 we have implicit the Core profile and Before 3.0 we have the
    // Compatibility profile
    // format.setProfile(QSurfaceFormat::CoreProfile);

    // setSwapInterval sets the application preferred swap interval
    // in minimum number of video frames that are displayed before a buffer swap occurs
    // - 0 will turn the vertical refresh syncing off
    // - 1 (default) means swapping after drawig a video frame to the buffer
    // - n means swapping after drawing n video frames to the buffer
    //
    // The vertical sync setting requested by the OpenGL application, can be overwritten
    // if a user changes the "Wait for vertical refresh" setting in AMD graphic drivers
    // for Windows.

#if defined(__APPLE__)
    // On OS X, syncing to vsync has good performance FPS-wise and
    // eliminates tearing. (This is an comment from pre QOpenGLWindow times)
    format.setSwapInterval(1);
    (void)vsyncMode;
#else
    // It seems that on Windows (at least for some AMD drivers), the setting 1 is not
    // not properly handled. We saw frame rates divided by exact integers, like it should
    // be with values >1 (see https://github.com/mixxxdj/mixxx/issues/11617)
    // Reported as https://bugreports.qt.io/browse/QTBUG-114882
    // On Linux, horrible FPS were seen with "VSync off" before switching to QOpenGLWindow too
    format.setSwapInterval(vsyncMode == VSyncThread::ST_PLL ? 1 : 0);
#endif
    return format;
}

void WaveformWidgetFactory::setUntilMarkShowBeats(bool value) {
    m_untilMarkShowBeats = value;
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]", "UntilMarkShowBeats"),
                ConfigValue(m_untilMarkShowBeats));
    }
}

void WaveformWidgetFactory::setUntilMarkShowTime(bool value) {
    m_untilMarkShowTime = value;
    if (m_config) {
        m_config->set(ConfigKey("[Waveform]", "UntilMarkShowTime"),
                ConfigValue(m_untilMarkShowTime));
    }
}

void WaveformWidgetFactory::setUntilMarkAlign(Qt::Alignment align) {
    m_untilMarkAlign = align;
    if (m_config) {
        m_config->setValue(ConfigKey("[Waveform]", "UntilMarkAlign"),
                toUntilMarkAlignIndex(m_untilMarkAlign));
    }
}
void WaveformWidgetFactory::setUntilMarkTextPointSize(int value) {
    m_untilMarkTextPointSize = value;
    if (m_config) {
        m_config->setValue(ConfigKey("[Waveform]", "UntilMarkTextPointSize"),
                m_untilMarkTextPointSize);
    }
}

// static
Qt::Alignment WaveformWidgetFactory::toUntilMarkAlign(int index) {
    switch (index) {
    case 0:
        return Qt::AlignTop;
    case 1:
        return Qt::AlignVCenter;
    case 2:
        return Qt::AlignBottom;
    }
    assert(false);
    return Qt::AlignVCenter;
}
// static
int WaveformWidgetFactory::toUntilMarkAlignIndex(Qt::Alignment align) {
    switch (align) {
    case Qt::AlignTop:
        return 0;
    case Qt::AlignVCenter:
        return 1;
    case Qt::AlignBottom:
        return 2;
    default:
        break;
    }
    assert(false);
    return 1;
}
