#pragma once

#include "waveform/widgets/glwaveformwidgetabstract.h"

class GLWaveformWidget : public GLWaveformWidgetAbstract {
    Q_OBJECT
  public:
    GLWaveformWidget(const QString& group, QWidget* parent);
    virtual ~GLWaveformWidget();

    virtual WaveformWidgetType::Type getType() const {
        return WaveformWidgetType::Filtered;
    }

    static inline bool useOpenGl() { return true; }
    static inline bool useOpenGles() { return false; }
    static inline bool useOpenGLShaders() { return false; }
    static inline WaveformWidgetCategory category() {
        return WaveformWidgetCategory::Legacy;
    }

  protected:
    virtual void castToQWidget();
    virtual void paintEvent(QPaintEvent* event);
    virtual mixxx::Duration render();

  private:
    friend class WaveformWidgetFactory;
};
