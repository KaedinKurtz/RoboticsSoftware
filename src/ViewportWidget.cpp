#include "ViewportWidget.hpp"
#include "Scene.hpp"
#include "components.hpp"
#include "Shader.hpp"
#include "Mesh.hpp"
#include "Camera.hpp"
#include "IntersectionSystem.hpp"
#include "RenderingSystem.hpp"
#include "DebugHelpers.hpp"

#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QSurfaceFormat>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QDebug>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <QMatrix4x4>
#include <utility> // For std::move
#include <iomanip>

// Recursive helper function for Forward Kinematics
glm::mat4 calculateMainWorldTransform(entt::entity entity, entt::registry& registry, int depth = 0)
{
    QString indent = QString(depth * 4, ' ');
    auto* tagPtr = registry.try_get<TagComponent>(entity);
    QString tag = tagPtr ? QString::fromStdString(tagPtr->tag) : "NO_TAG";
    qDebug().noquote() << indent << "[MainFK] Calculating for" << entity << "tagged as" << tag;

    auto& transformComp = registry.get<TransformComponent>(entity);
    glm::mat4 localTransform = transformComp.getTransform();

    glm::mat4 finalTransform = localTransform;

    if (registry.all_of<ParentComponent>(entity)) {
        auto& parentComp = registry.get<ParentComponent>(entity);
        if (registry.valid(parentComp.parent)) {
            qDebug().noquote() << indent << "  -> Found parent" << parentComp.parent << ". Recursing...";
            glm::mat4 parentWorldTransform = calculateMainWorldTransform(parentComp.parent, registry, depth + 1);
            finalTransform = parentWorldTransform * localTransform;
        }
    }
    return finalTransform;
}

ViewportWidget::ViewportWidget(Scene* scene, entt::entity cameraEntity, QWidget* parent)
    : QOpenGLWidget(parent),
    m_scene(scene),
    m_cameraEntity(cameraEntity),
    m_animationTimer(new QTimer(this)),
    m_outlineVAO(0),
    m_outlineVBO(0),
    m_cleanedUp(false)
{
    // Request a debug context so we can attach QOpenGLDebugLogger later
    QSurfaceFormat fmt = format();
    fmt.setOption(QSurfaceFormat::DebugContext);
    setFormat(fmt);

    Q_ASSERT(m_scene != nullptr);
    setFocusPolicy(Qt::StrongFocus);

}

ViewportWidget::~ViewportWidget() = default;

void ViewportWidget::cleanupGL()
{
    if (m_cleanedUp)
        return;
    m_cleanedUp = true;

    // 1) Stop the debug logger (if you want to be double-sure):
    if (m_debugLogger && m_debugLogger->isLogging()) {
        m_debugLogger->stopLogging();
    }

    // 2) Tear down any widget-local buffers
    if (m_outlineVBO) {
        glDeleteBuffers(1, &m_outlineVBO);
        m_outlineVBO = 0;
    }
    if (m_outlineVAO) {
        glDeleteVertexArrays(1, &m_outlineVAO);
        m_outlineVAO = 0;
    }

    // 3) Shut down your global RenderingSystem
    RenderingSystem::shutdown(m_scene);
}

Camera& ViewportWidget::getCamera()
{
    return m_scene->getRegistry().get<CameraComponent>(m_cameraEntity).camera;
}

void ViewportWidget::initializeGL()
{
    // 1) Initialize core functions
    initializeOpenGLFunctions();

    if (!context()) {
        qWarning() << "[ViewportWidget] initializeGL() called without a current context!";
        return;
    }

    // 2) Set up debug logger (if supported)
    m_debugLogger = std::make_unique<QOpenGLDebugLogger>(this);
    if (m_debugLogger->initialize()) {
        // a) Stop logging before the context is destroyed
        connect(context(), &QOpenGLContext::aboutToBeDestroyed,
            m_debugLogger.get(), &QOpenGLDebugLogger::stopLogging,
            Qt::DirectConnection);

        // b) Also perform our GL cleanup while the context is still alive
        connect(context(), &QOpenGLContext::aboutToBeDestroyed,
            this, [this]() {
                makeCurrent();
                cleanupGL();
                doneCurrent();
            },
            Qt::DirectConnection);

        // c) Print debug messages
        connect(m_debugLogger.get(), &QOpenGLDebugLogger::messageLogged,
            this, [](const QOpenGLDebugMessage& msg) {
                qDebug() << "[GL Debug]" << msg;
            });

        m_debugLogger->startLogging(QOpenGLDebugLogger::SynchronousLogging);
    }
    else {
        qWarning() << "[ViewportWidget] OpenGL debug logger unavailable.";
    }

    // 3) Standard GL state
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 4) Load your outline shader and initialize the global rendering system
    try {
        m_outlineShader = std::make_unique<Shader>(
            this,
            "shaders/outline_vert.glsl",
            "shaders/outline_frag.glsl"
        );
        RenderingSystem::initialize();
    }
    catch (const std::exception& e) {
        qCritical() << "[ViewportWidget] CRITICAL: Failed to initialize shaders or RenderingSystem:"
            << e.what();
    }

    // 5) Create VAO/VBO for your outline pass
    glGenVertexArrays(1, &m_outlineVAO);
    glGenBuffers(1, &m_outlineVBO);
    glBindVertexArray(m_outlineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_outlineVBO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,                      // attribute 0
        3,                      // vec3
        GL_FLOAT,
        GL_FALSE,
        sizeof(glm::vec3),
        (void*)0
    );
    glBindVertexArray(0);

    // 6) Kick off your animation timer
    connect(m_animationTimer, &QTimer::timeout, this, QOverload<>::of(&ViewportWidget::update));
    m_animationTimer->start(16);
}


void ViewportWidget::paintGL()
{
    IntersectionSystem::update(m_scene);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!m_scene) return;
    auto& registry = m_scene->getRegistry();
    auto& camera = getCamera();
    const float aspectRatio = (height() > 0) ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const glm::mat4 viewMatrix = camera.getViewMatrix();
    const glm::mat4 projectionMatrix = camera.getProjectionMatrix(aspectRatio);

    RenderingSystem::render(m_scene, viewMatrix, projectionMatrix, camera.getPosition());

    // --- Draw Intersection Outlines ---
    if (m_outlineShader && m_outlineVAO != 0) {
        // ... (this logic was correct)
    }
}

void ViewportWidget::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);
}

void ViewportWidget::mousePressEvent(QMouseEvent* event)
{
    m_lastMousePos = event->pos();
    QOpenGLWidget::mousePressEvent(event);
}

void ViewportWidget::mouseMoveEvent(QMouseEvent* event)
{
    int dx = event->pos().x() - m_lastMousePos.x();
    int dy = event->pos().y() - m_lastMousePos.y();
    bool isPanning = (event->buttons() & Qt::MiddleButton);
    bool isOrbiting = (event->buttons() & Qt::LeftButton);

    if (isOrbiting || isPanning) {
        getCamera().processMouseMovement(dx, -dy, isPanning);
    }
    m_lastMousePos = event->pos();
    QOpenGLWidget::mouseMoveEvent(event);
}

void ViewportWidget::wheelEvent(QWheelEvent* event)
{
    getCamera().processMouseScroll(event->angleDelta().y() / 120.0f);
    QOpenGLWidget::wheelEvent(event);
}

void ViewportWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_P) {
        getCamera().toggleProjection();
    }
    else if (event->key() == Qt::Key_R) {
        getCamera().setToKnownGoodView();
    }
    QOpenGLWidget::keyPressEvent(event);
}

void ViewportWidget::closeEvent(QCloseEvent* event)
{
    // If someone closes the widget directly, we still want a safe cleanup:
    if (context()) {
        makeCurrent();
        try {
            cleanupGL();
        }
        catch (const std::exception& ex) {
            qWarning() << "[ViewportWidget] exception during cleanupGL():" << ex.what();
        }
        catch (...) {
            qWarning() << "[ViewportWidget] unknown error during cleanupGL()";
        }
        doneCurrent();
    }
    QOpenGLWidget::closeEvent(event);
}
