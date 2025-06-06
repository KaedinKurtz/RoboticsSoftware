/**
 * @file ViewportWidget.cpp
 * @brief Implementation of a QOpenGLWidget for rendering a scene from a specific camera's perspective.
 */

#include "ViewportWidget.hpp"
#include "Scene.hpp"
#include "components.hpp"
#include "Shader.hpp"
#include "Mesh.hpp"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QDebug>


ViewportWidget::ViewportWidget(Scene* scene, entt::entity cameraEntity, QWidget* parent)
    : QOpenGLWidget(parent),
    m_scene(scene),
    m_cameraEntity(cameraEntity)
{
    Q_ASSERT(m_scene != nullptr);
    qDebug() << "ViewportWidget CONSTRUCTOR for camera entity" << (uint32_t)m_cameraEntity;
    setFocusPolicy(Qt::StrongFocus);
}

ViewportWidget::~ViewportWidget()
{
    qDebug() << "ViewportWidget DESTRUCTOR for camera entity" << (uint32_t)m_cameraEntity;
    // Disconnect the cleanup signal to prevent a call on a deleted object
    // if the widget is destroyed before the context.
    if (m_glContext) {
        disconnect(m_glContext, &QOpenGLContext::aboutToBeDestroyed, this, &ViewportWidget::cleanup);
    }
}

// --- FIX: Implementation of the cleanup slot ---
// This function is called by the aboutToBeDestroyed signal. It is guaranteed
// to run while the correct OpenGL context is still current, making it the
// safe place to delete OpenGL resources.
void ViewportWidget::cleanup()
{
    qDebug() << "Context cleanup for" << this;
    // Make the context current to be absolutely sure we can delete resources.
    makeCurrent();
    m_gridShader.reset();
    m_gridMesh.reset();
    m_phongShader.reset();
    m_cubeMesh.reset();
    doneCurrent();
}


Camera& ViewportWidget::getCamera()
{
    return m_scene->getRegistry().get<CameraComponent>(m_cameraEntity).camera;
}

const Camera& ViewportWidget::getCamera() const
{
    return m_scene->getRegistry().get<CameraComponent>(m_cameraEntity).camera;
}

void ViewportWidget::initializeGL() {
    // --- FIX: Connect context destruction signal to our cleanup slot ---
    // This is the core of the fix. It ties the lifetime of our OpenGL resources
    // directly to the lifetime of the context that created them.
    m_glContext = this->context();
    connect(m_glContext, &QOpenGLContext::aboutToBeDestroyed, this, &ViewportWidget::cleanup, Qt::DirectConnection);

    initializeOpenGLFunctions();
    qDebug() << "ViewportWidget INITIALIZEGL for camera entity" << (uint32_t)m_cameraEntity << "with context" << m_glContext;
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    try {
        m_gridShader = std::make_unique<Shader>(this, "shaders/grid_vert.glsl", "shaders/grid_frag.glsl");
        m_phongShader = std::make_unique<Shader>(this, "shaders/vertex_shader.glsl", "shaders/fragment_shader.glsl");

        const float halfSize = 1000.0f;
        const std::vector<float> quad_vertices = {
            -halfSize, 0.0f, -halfSize,  halfSize, 0.0f, -halfSize,
             halfSize, 0.0f,  halfSize,  halfSize, 0.0f,  halfSize,
            -halfSize, 0.0f,  halfSize, -halfSize, 0.0f, -halfSize
        };
        m_gridMesh = std::make_unique<Mesh>(this, quad_vertices);

        const std::vector<float> cube_vertices = {
            -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,
             0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f, -0.5f, -0.5f,
            -0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f,
             0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,
            -0.5f,  0.5f,  0.5f, -0.5f,  0.5f, -0.5f, -0.5f, -0.5f, -0.5f,
            -0.5f, -0.5f, -0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f,
             0.5f,  0.5f,  0.5f,  0.5f,  0.5f, -0.5f,  0.5f, -0.5f, -0.5f,
             0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,  0.5f,
            -0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,  0.5f,
             0.5f, -0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f, -0.5f,
            -0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f,  0.5f,
             0.5f,  0.5f,  0.5f, -0.5f,  0.5f,  0.5f, -0.5f,  0.5f, -0.5f
        };
        m_cubeMesh = std::make_unique<Mesh>(this, cube_vertices);

    }
    catch (const std::exception& e) {
        qCritical() << "Failed to initialize resources in ViewportWidget:" << e.what();
    }

    if (!m_animationTimer.isActive()) {
        connect(&m_animationTimer, &QTimer::timeout, this, QOverload<>::of(&ViewportWidget::update));
        m_animationTimer.start(1000 / 60);
    }
}

void ViewportWidget::paintGL() {
    checkGLError("Start of paintGL");
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!m_scene) return;

    auto& registry = m_scene->getRegistry();
    const auto& camera = getCamera();
    const float aspectRatio = (height() > 0) ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const glm::mat4 viewMatrix = camera.getViewMatrix();
    const glm::mat4 projectionMatrix = camera.getProjectionMatrix(aspectRatio);

    const auto& sceneProps = registry.ctx().get<SceneProperties>();

    // --- 1. Draw all Grid Entities ---
    if (m_gridShader && m_gridMesh) {
        m_gridShader->use();

        m_gridShader->setMat4("u_viewMatrix", viewMatrix);
        m_gridShader->setMat4("u_projectionMatrix", projectionMatrix);
        m_gridShader->setVec3("u_cameraPos", camera.getPosition());
        m_gridShader->setFloat("u_cameraDistanceToFocal", camera.getDistance());
        m_gridShader->setBool("u_useFog", sceneProps.fogEnabled);
        m_gridShader->setVec3("u_fogColor", sceneProps.fogColor);
        m_gridShader->setFloat("u_fogStartDistance", sceneProps.fogStartDistance);
        m_gridShader->setFloat("u_fogEndDistance", sceneProps.fogEndDistance);

        auto gridView = registry.view<const TransformComponent, const GridComponent>();
        gridView.each([this](const auto& transform, const auto& grid) {
            if (!grid.visible) {
                return;
            }
            m_gridShader->setMat4("u_gridModelMatrix", transform.getTransform());
            const int numLevelsToSend = static_cast<int>(grid.levels.size());
            m_gridShader->setInt("u_numLevels", numLevelsToSend);
            for (int i = 0; i < numLevelsToSend; ++i) {
                std::string base = "u_levels[" + std::to_string(i) + "].";
                m_gridShader->setFloat((base + "spacing").c_str(), grid.levels[i].spacing);
                m_gridShader->setVec3((base + "color").c_str(), grid.levels[i].color);
                m_gridShader->setFloat((base + "fadeInCameraDistanceEnd").c_str(), grid.levels[i].fadeInCameraDistanceEnd);
                m_gridShader->setFloat((base + "fadeInCameraDistanceStart").c_str(), grid.levels[i].fadeInCameraDistanceStart);
            }
            m_gridShader->setBool("u_showAxes", grid.showAxes);
            m_gridShader->setVec3("u_xAxisColor", grid.xAxisColor);
            m_gridShader->setVec3("u_zAxisColor", grid.zAxisColor);
            m_gridShader->setFloat("u_axisLineWidthPixels", grid.axisLineWidthPixels);
            m_gridShader->setFloat("u_baseLineWidthPixels", grid.baseLineWidthPixels);
            m_gridMesh->draw();
            });
    }

    // --- 2. Draw all Renderable Mesh Entities ---
    if (m_phongShader && m_cubeMesh) {
        m_phongShader->use();
        m_phongShader->setMat4("view", viewMatrix);
        m_phongShader->setMat4("projection", projectionMatrix);

        auto meshView = registry.view<const TransformComponent, const RenderableMeshComponent>();
        meshView.each([this](const auto& transform, const auto& renderable) {
            m_phongShader->setMat4("model", transform.getTransform());
            m_cubeMesh->draw();
            });
    }

    checkGLError("End of paintGL");
}

void ViewportWidget::resizeGL(int w, int h) {
    if (w <= 0 || h <= 0) return;
    glViewport(0, 0, w, h);
}

void ViewportWidget::mousePressEvent(QMouseEvent* event) {
    m_lastMousePos = event->pos();
    QOpenGLWidget::mousePressEvent(event);
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event) {
    int dx = event->pos().x() - m_lastMousePos.x();
    int dy = event->pos().y() - m_lastMousePos.y();
    bool isPanning = (event->buttons() & Qt::MiddleButton);
    bool isOrbiting = (event->buttons() & Qt::LeftButton);

    if (isOrbiting || isPanning) {
        getCamera().processMouseMovement(dx, -dy, isPanning);
        update();
    }
    m_lastMousePos = event->pos();
    QOpenGLWidget::mouseMoveEvent(event);
}

void ViewportWidget::wheelEvent(QWheelEvent* event) {
    getCamera().processMouseScroll(event->angleDelta().y() / 120.0f);
    update();
    QOpenGLWidget::wheelEvent(event);
}

void ViewportWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_P) {
        getCamera().toggleProjection();
        update();
    }
    else if (event->key() == Qt::Key_R) {
        getCamera().setToKnownGoodView();
        update();
    }
    QOpenGLWidget::keyPressEvent(event);
}

void ViewportWidget::checkGLError(const char* location) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        qDebug() << "OpenGL error at" << location << "- Code:" << Qt::hex << err;
    }
}
