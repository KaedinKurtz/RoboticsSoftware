#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLDebugLogger>
#include <QMetaObject>
#include <QPoint>
#include <memory>
#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

// Forward declarations
class Scene;
class Shader;
class Mesh;
class Camera;
class QTimer;
class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QCloseEvent;


class ViewportWidget : public QOpenGLWidget, public QOpenGLFunctions_3_3_Core
{
    Q_OBJECT

public:
    ViewportWidget(Scene* scene, entt::entity cameraEntity, QWidget* parent = nullptr);
    ~ViewportWidget() override;

    Camera& getCamera();

private slots:
    void cleanupGL();

protected:
    // Overrides from QOpenGLWidget
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    Scene* m_scene;
    entt::entity m_cameraEntity;
    QOpenGLDebugLogger* m_debugLogger = nullptr;

    // --- INTEGRATED: OpenGL resources for intersection outline rendering ---
    std::unique_ptr<Shader> m_outlineShader; // The shader program for drawing simple colored lines.
    unsigned int m_outlineVAO;               // The Vertex Array Object for the outline geometry.
    unsigned int m_outlineVBO;               // The Vertex Buffer Object for the outline geometry.

    // Other members
    QTimer* m_animationTimer = nullptr;
    QPoint m_lastMousePos;

    // Emits detailed OpenGL debug output when a debug context is available
    std::unique_ptr<QOpenGLDebugLogger> m_debugLogger;  // <-- use unique_ptr, not raw pointer


    // Guard to ensure cleanupGL() is only executed once
    bool m_cleanedUp = false;
};
