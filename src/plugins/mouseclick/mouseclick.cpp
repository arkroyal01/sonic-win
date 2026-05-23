/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2012 Filip Wieladek <wattos@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "mouseclick.h"
// KConfigSkeleton
#include "mouseclickconfig.h"

#include "config-kwin.h"
#include "core/inputdevice.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "input_event.h"
#include "scene/workspacescene.h"

#include <QAction>

#include <KConfigGroup>
#include <KGlobalAccel>

#include <QPainter>

#include <cmath>

#if HAVE_VULKAN
#include "platformsupport/scenes/vulkan/vulkanbackend.h"
#include "platformsupport/scenes/vulkan/vulkanbuffer.h"
#include "platformsupport/scenes/vulkan/vulkancontext.h"
#include "platformsupport/scenes/vulkan/vulkanpipeline.h"
#include "platformsupport/scenes/vulkan/vulkanpipelinemanager.h"
#include "platformsupport/scenes/vulkan/vulkantexture.h"
#include "scene/itemrenderer_vulkan.h"
#include <array>
#include <vulkan/vulkan.h>
#endif

namespace KWin
{

MouseClickEffect::MouseClickEffect()
{
    MouseClickConfig::instance(effects->config());
    m_enabled = false;

    QAction *a = new QAction(this);
    a->setObjectName(QStringLiteral("ToggleMouseClick"));
    a->setText(i18n("Toggle Mouse Click Animation"));
    KGlobalAccel::self()->setDefaultShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Asterisk));
    KGlobalAccel::self()->setShortcut(a, QList<QKeySequence>() << (Qt::META | Qt::Key_Asterisk));
    connect(a, &QAction::triggered, this, &MouseClickEffect::toggleEnabled);

    reconfigure(ReconfigureAll);

    m_buttons[0] = std::make_unique<MouseButton>(i18nc("Left mouse button", "Left"), Qt::LeftButton);
    m_buttons[1] = std::make_unique<MouseButton>(i18nc("Middle mouse button", "Middle"), Qt::MiddleButton);
    m_buttons[2] = std::make_unique<MouseButton>(i18nc("Right mouse button", "Right"), Qt::RightButton);
}

MouseClickEffect::~MouseClickEffect()
{
}

void MouseClickEffect::reconfigure(ReconfigureFlags)
{
    MouseClickConfig::self()->read();
    m_colors[0] = MouseClickConfig::color1();
    m_colors[1] = MouseClickConfig::color2();
    m_colors[2] = MouseClickConfig::color3();
    m_lineWidth = MouseClickConfig::lineWidth();
    m_ringLife = MouseClickConfig::ringLife();
    m_ringMaxSize = MouseClickConfig::ringSize();
    m_ringCount = MouseClickConfig::ringCount();
    m_showText = MouseClickConfig::showText();
    m_font = MouseClickConfig::font();
}

void MouseClickEffect::prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    const int time = m_lastPresentTime.count() ? (presentTime - m_lastPresentTime).count() : 0;

    for (auto &click : m_clicks) {
        click->m_time += time;
    }

    for (int i = 0; i < BUTTON_COUNT; ++i) {
        if (m_buttons[i]->m_isPressed) {
            m_buttons[i]->m_time += time;
        }
    }

    while (m_clicks.size() > 0) {
        if (m_clicks.front()->m_time <= m_ringLife) {
            break;
        }
        m_clicks.pop_front();
    }

    if (isActive()) {
        m_lastPresentTime = presentTime;
    } else {
        m_lastPresentTime = std::chrono::milliseconds::zero();
    }

    effects->prePaintScreen(data, presentTime);
}

void MouseClickEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen);

    if (effects->isOpenGLCompositing()) {
        paintScreenSetupGl(renderTarget, viewport.projectionMatrix());
    }
    for (const auto &click : m_clicks) {
        for (int i = 0; i < m_ringCount; ++i) {
            float alpha = computeAlpha(click.get(), i);
            float size = computeRadius(click.get(), i);
            if (size > 0 && alpha > 0) {
                QColor color = m_colors[click->m_button];
                color.setAlphaF(alpha);
                drawCircle(renderTarget, viewport, color, click->m_pos.x(), click->m_pos.y(), size);
            }
        }

        if (m_showText && click->m_frame) {
            float frameAlpha = (click->m_time * 2.0f - m_ringLife) / m_ringLife;
            frameAlpha = frameAlpha < 0 ? 1 : -(frameAlpha * frameAlpha) + 1;
            click->m_frame->render(renderTarget, viewport, infiniteRegion(), frameAlpha, frameAlpha);
        }
    }
    for (const auto &tool : std::as_const(m_tabletTools)) {
        const int step = m_ringMaxSize * (1. - tool.m_pressure);
        for (qreal size = m_ringMaxSize; size > 0; size -= step) {
            drawCircle(renderTarget, viewport, tool.m_color, tool.m_globalPosition.x(), tool.m_globalPosition.y(), size);
        }
    }
    if (effects->isOpenGLCompositing()) {
        paintScreenFinishGl();
    }
}

void MouseClickEffect::postPaintScreen()
{
    effects->postPaintScreen();
    repaint();
}

float MouseClickEffect::computeRadius(const MouseClickMouseEvent *click, int ring)
{
    float ringDistance = m_ringLife / (m_ringCount * 3);
    if (click->m_press) {
        return ((click->m_time - ringDistance * ring) / m_ringLife) * m_ringMaxSize;
    }
    return ((m_ringLife - click->m_time - ringDistance * ring) / m_ringLife) * m_ringMaxSize;
}

float MouseClickEffect::computeAlpha(const MouseClickMouseEvent *click, int ring)
{
    float ringDistance = m_ringLife / (m_ringCount * 3);
    return (m_ringLife - (float)click->m_time - ringDistance * (ring)) / m_ringLife;
}

void MouseClickEffect::slotMouseChanged(const QPointF &pos, const QPointF &,
                                        Qt::MouseButtons buttons, Qt::MouseButtons oldButtons,
                                        Qt::KeyboardModifiers, Qt::KeyboardModifiers)
{
    if (buttons == oldButtons) {
        return;
    }

    std::unique_ptr<MouseClickMouseEvent> m;
    int i = BUTTON_COUNT;
    while (--i >= 0) {
        MouseButton *b = m_buttons[i].get();
        if (isPressed(b->m_button, buttons, oldButtons)) {
            m = std::make_unique<MouseClickMouseEvent>(i, pos.toPoint(), 0, createEffectFrame(pos.toPoint(), b->m_labelDown), true);
            break;
        } else if (isReleased(b->m_button, buttons, oldButtons) && (!b->m_isPressed || b->m_time > m_ringLife)) {
            // we might miss a press, thus also check !b->m_isPressed, bug #314762
            m = std::make_unique<MouseClickMouseEvent>(i, pos.toPoint(), 0, createEffectFrame(pos.toPoint(), b->m_labelUp), false);
            break;
        }
        b->setPressed(b->m_button & buttons);
    }

    if (m) {
        m_clicks.push_back(std::move(m));
    }
    repaint();
}

std::unique_ptr<EffectFrame> MouseClickEffect::createEffectFrame(const QPoint &pos, const QString &text)
{
    if (!m_showText) {
        return nullptr;
    }
    QPoint point(pos.x() + m_ringMaxSize, pos.y());
    std::unique_ptr<EffectFrame> frame = std::make_unique<EffectFrame>(EffectFrameStyled, false, point, Qt::AlignLeft);
    frame->setFont(m_font);
    frame->setText(text);
    return frame;
}

void MouseClickEffect::repaint()
{
    if (m_clicks.size() > 0) {
        QRegion dirtyRegion;
        const int radius = m_ringMaxSize + m_lineWidth;
        for (auto &click : m_clicks) {
            dirtyRegion += QRect(click->m_pos.x() - radius, click->m_pos.y() - radius, 2 * radius, 2 * radius);
            if (click->m_frame) {
                dirtyRegion += click->m_frame->geometry();
            }
        }
        effects->addRepaint(dirtyRegion);
    }
    if (!m_tabletTools.isEmpty()) {
        QRegion dirtyRegion;
        const int radius = m_ringMaxSize + m_lineWidth;
        for (const auto &event : std::as_const(m_tabletTools)) {
            dirtyRegion += QRect(event.m_globalPosition.x() - radius, event.m_globalPosition.y() - radius, 2 * radius, 2 * radius);
        }
        effects->addRepaint(dirtyRegion);
    }
}

bool MouseClickEffect::isReleased(Qt::MouseButtons button, Qt::MouseButtons buttons, Qt::MouseButtons oldButtons)
{
    return !(button & buttons) && (button & oldButtons);
}

bool MouseClickEffect::isPressed(Qt::MouseButtons button, Qt::MouseButtons buttons, Qt::MouseButtons oldButtons)
{
    return (button & buttons) && !(button & oldButtons);
}

void MouseClickEffect::toggleEnabled()
{
    m_enabled = !m_enabled;

    if (m_enabled) {
        connect(effects, &EffectsHandler::mouseChanged, this, &MouseClickEffect::slotMouseChanged);
    } else {
        disconnect(effects, &EffectsHandler::mouseChanged, this, &MouseClickEffect::slotMouseChanged);
    }

    m_clicks.clear();
    m_tabletTools.clear();

    for (int i = 0; i < BUTTON_COUNT; ++i) {
        m_buttons[i]->m_time = 0;
        m_buttons[i]->m_isPressed = false;
    }
}

bool MouseClickEffect::isActive() const
{
    return m_enabled && (m_clicks.size() != 0 || !m_tabletTools.isEmpty());
}

void MouseClickEffect::drawCircle(const RenderTarget &renderTarget, const RenderViewport &viewport, const QColor &color, float cx, float cy, float r)
{
    if (effects->isOpenGLCompositing()) {
        drawCircleGl(viewport, color, cx, cy, r);
    }
#if HAVE_VULKAN
    else if (effects->isVulkanCompositing()) {
        drawCircleVulkan(renderTarget, viewport, color, cx, cy, r);
    }
#endif
}

void MouseClickEffect::drawCircleGl(const RenderViewport &viewport, const QColor &color, float cx, float cy, float r)
{
    static const int num_segments = 80;
    static const float theta = 2 * 3.1415926 / float(num_segments);
    static const float c = cosf(theta); // precalculate the sine and cosine
    static const float s = sinf(theta);
    const float scale = viewport.scale();
    float t;

    float x = r; // we start at angle = 0
    float y = 0;

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    QList<QVector2D> verts;
    verts.reserve(num_segments * 2);

    for (int ii = 0; ii < num_segments; ++ii) {
        verts.push_back(QVector2D((x + cx) * scale, (y + cy) * scale)); // output vertex
        // apply the rotation matrix
        t = x;
        x = c * x - s * y;
        y = s * t + c * y;
    }
    vbo->setVertices(verts);
    GLShaderManager::instance()->getBoundShader()->setUniform(GLShader::ColorUniform::Color, color);
    vbo->render(GL_LINE_LOOP);
}

void MouseClickEffect::drawCircleQPainter(const QColor &color, float cx, float cy, float r)
{
    QPainter *painter = effects->scenePainter();
    painter->save();
    painter->setPen(color);
    painter->drawArc(cx - r, cy - r, r * 2, r * 2, 0, 5760);
    painter->restore();
}

void MouseClickEffect::paintScreenSetupGl(const RenderTarget &renderTarget, const QMatrix4x4 &projectionMatrix)
{
    GLShader *shader = GLShaderManager::instance()->pushShader(GLShaderTrait::UniformColor | GLShaderTrait::TransformColorspace);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, projectionMatrix);
    shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

    glLineWidth(m_lineWidth);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void MouseClickEffect::paintScreenFinishGl()
{
    glDisable(GL_BLEND);

    GLShaderManager::instance()->popShader();
}

TabletToolEvent &MouseClickEffect::getOrCreateTabletPoint(InputDeviceTabletTool *tool)
{
    auto &point = m_tabletTools[tool];
    if (!point.m_color.isValid()) {
        switch (tool->type()) {
        case InputDeviceTabletTool::Type::Finger:
        case InputDeviceTabletTool::Type::Pen:
        case InputDeviceTabletTool::Pencil:
        case InputDeviceTabletTool::Brush:
        case InputDeviceTabletTool::Airbrush:
        case InputDeviceTabletTool::Lens:
        case InputDeviceTabletTool::Totem:
            point.m_color = MouseClickConfig::color1();
            break;
        case InputDeviceTabletTool::Type::Eraser:
            point.m_color = MouseClickConfig::color2();
            break;
        case KWin::InputDeviceTabletTool::Type::Mouse:
            point.m_color = MouseClickConfig::color3();
            break;
            break;
        }
    }
    return point;
}

#if HAVE_VULKAN
void MouseClickEffect::drawCircleVulkan(const RenderTarget &renderTarget, const RenderViewport &viewport, const QColor &color, float cx, float cy, float r)
{
    VulkanContext *ctx = VulkanContext::currentContext();
    if (!ctx) {
        return;
    }
    auto *vkRenderer = dynamic_cast<ItemRendererVulkan *>(effects->scene()->renderer());
    if (!vkRenderer) {
        return;
    }
    VkCommandBuffer cmd = vkRenderer->activeCommandBuffer(renderTarget);
    if (cmd == VK_NULL_HANDLE) {
        return;
    }
    VulkanTexture *whiteTex = vkRenderer->defaultWhiteTexture();
    if (!whiteTex || !whiteTex->isValid()) {
        return;
    }
    VulkanPipeline *pipeline = ctx->pipelineManager()->pipeline(VulkanShaderTrait::UniformColor);
    if (!pipeline || !pipeline->isValid()) {
        return;
    }

    // Tessellate the circle outline as 80 thin quads (one per segment).
    // Each quad is two triangles with half-width = lineWidth/2 perpendicular to the segment.
    static const int kSegments = 80;
    static const float kTheta = 2.0f * M_PI / kSegments;
    const float scale = viewport.scale();
    const float halfW = m_lineWidth * 0.5f;

    QList<VulkanVertex2D> verts;
    verts.reserve(kSegments * 6);

    float px = cx + r, py = cy; // previous point
    for (int i = 1; i <= kSegments; ++i) {
        const float angle = kTheta * i;
        const float qx = cx + r * std::cos(angle);
        const float qy = cy + r * std::sin(angle);

        // Segment direction and perpendicular normal
        const float dx = qx - px, dy = qy - py;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) {
            px = qx;
            py = qy;
            continue;
        }
        const float nx = -dy / len * halfW, ny = dx / len * halfW;

        // Quad corners (in logical pixels, scaled to device pixels)
        const float ax = (px - nx) * scale, ay = (py - ny) * scale;
        const float bx = (px + nx) * scale, by = (py + ny) * scale;
        const float cx2 = (qx - nx) * scale, cy2 = (qy - ny) * scale;
        const float dx2 = (qx + nx) * scale, dy2 = (qy + ny) * scale;

        verts.push_back({{ax, ay}, {0.0f, 0.0f}});
        verts.push_back({{bx, by}, {0.0f, 1.0f}});
        verts.push_back({{cx2, cy2}, {1.0f, 0.0f}});
        verts.push_back({{bx, by}, {0.0f, 1.0f}});
        verts.push_back({{dx2, dy2}, {1.0f, 1.0f}});
        verts.push_back({{cx2, cy2}, {1.0f, 0.0f}});

        px = qx;
        py = qy;
    }

    auto vertBuf = VulkanBuffer::createStreamingBuffer(ctx, verts.size() * sizeof(VulkanVertex2D));
    if (!vertBuf) {
        return;
    }
    vertBuf->upload(verts.constData(), verts.size() * sizeof(VulkanVertex2D));

    VulkanPushConstants pc{};
    const QMatrix4x4 mvp = viewport.projectionMatrix();
    memcpy(pc.mvp, mvp.constData(), sizeof(pc.mvp));
    pc.textureMatrix[0] = pc.textureMatrix[5] = pc.textureMatrix[10] = pc.textureMatrix[15] = 1.0f;

    // Premultiplied RGBA for the UniformColor pipeline
    VulkanUniforms ubo{};
    const float alpha = color.alphaF();
    ubo.uniformColor[0] = color.redF() * alpha;
    ubo.uniformColor[1] = color.greenF() * alpha;
    ubo.uniformColor[2] = color.blueF() * alpha;
    ubo.uniformColor[3] = alpha;
    ubo.opacity = 1.0f;
    ubo.brightness = 1.0f;
    ubo.saturation = 1.0f;

    auto uboBuf = VulkanBuffer::createUniformBuffer(ctx, sizeof(VulkanUniforms));
    if (!uboBuf) {
        return;
    }
    uboBuf->upload(&ubo, sizeof(ubo));

    const VkDescriptorImageInfo imgInfo{whiteTex->sampler(), whiteTex->imageView(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const std::array<VkDescriptorImageInfo, 4> imageInfos{imgInfo, imgInfo, imgInfo, imgInfo};
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = uboBuf->buffer();
    bufInfo.offset = 0;
    bufInfo.range = sizeof(VulkanUniforms);

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 4;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = imageInfos.data();
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline());
    vkCmdPushConstants(cmd, pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(VulkanPushConstants), &pc);
    if (!ctx->bindDescriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline->layout(), pipeline->descriptorSetLayout(),
                              0, writes.size(), writes.data())) {
        return;
    }

    const VkBuffer vb = vertBuf->buffer();
    const VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
    vkCmdDraw(cmd, verts.size(), 1, 0, 0);
}
#endif

bool MouseClickEffect::tabletToolProximity(TabletToolProximityEvent *event)
{
    if (event->type == TabletToolProximityEvent::LeaveProximity) {
        m_tabletTools.remove(event->tool);
    }
    return false;
}

bool MouseClickEffect::tabletToolAxis(TabletToolAxisEvent *event)
{
    auto &point = getOrCreateTabletPoint(event->tool);
    point.m_globalPosition = event->position;
    point.m_pressure = event->pressure;
    return false;
}

bool MouseClickEffect::tabletToolTip(TabletToolTipEvent *event)
{
    auto &point = getOrCreateTabletPoint(event->tool);
    point.m_pressed = event->type == TabletToolTipEvent::Press;
    point.m_pressure = event->pressure;
    point.m_globalPosition = event->position;
    return false;
}

QColor MouseClickEffect::color1() const
{
    return m_colors[0];
}

QColor MouseClickEffect::color2() const
{
    return m_colors[1];
}

QColor MouseClickEffect::color3() const
{
    return m_colors[2];
}

qreal MouseClickEffect::lineWidth() const
{
    return m_lineWidth;
}

int MouseClickEffect::ringLife() const
{
    return m_ringLife;
}

int MouseClickEffect::ringSize() const
{
    return m_ringMaxSize;
}

int MouseClickEffect::ringCount() const
{
    return m_ringCount;
}

bool MouseClickEffect::isShowText() const
{
    return m_showText;
}

QFont MouseClickEffect::font() const
{
    return m_font;
}

bool MouseClickEffect::isEnabled() const
{
    return m_enabled;
}

} // namespace

#include "moc_mouseclick.cpp"
