#include "shaders/rgbashader.h"

using namespace mixxx;

void RGBAShader::init() {
    QString vertexShaderCode = QStringLiteral(R"--(
uniform mat4 matrix;
attribute highp vec4 position;
attribute highp vec4 color;
varying highp vec4 vColor;
void main()
{
    vColor = color;
    gl_Position = matrix * position;
}
)--");

    QString fragmentShaderCode = QStringLiteral(R"--(
varying highp vec4 vColor;
void main()
{
    gl_FragColor = vColor;
}
)--");

    load(vertexShaderCode, fragmentShaderCode);

    m_matrixLocation = uniformLocation("matrix");
    m_positionLocation = attributeLocation("position");
    m_colorLocation = attributeLocation("color");
}
