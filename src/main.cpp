#include <sp2/engine.h>
#include <sp2/window.h>
#include <sp2/logging.h>
#include <sp2/random.h>
#include <sp2/tweak.h>
#include <sp2/io/resourceProvider.h>
#include <sp2/io/directoryResourceProvider.h>
#include <sp2/graphics/gui/scene.h>
#include <sp2/graphics/gui/theme.h>
#include <sp2/graphics/gui/loader.h>
#include <sp2/graphics/gui/widget/label.h>
#include <sp2/graphics/gui/widget/panel.h>
#include <sp2/graphics/scene/graphicslayer.h>
#include <sp2/graphics/scene/basicnoderenderpass.h>
#include <sp2/graphics/scene/collisionrenderpass.h>
#include <sp2/graphics/textureManager.h>
#include <sp2/graphics/spriteAnimation.h>
#include <sp2/graphics/meshdata.h>
#include <sp2/graphics/meshbuilder.h>
#include <sp2/graphics/mesh/obj.h>
#include <sp2/scene/scene.h>
#include <sp2/scene/node.h>
#include <sp2/scene/tilemap.h>
#include <sp2/scene/camera.h>
#include <sp2/scene/particleEmitter.h>
#include <sp2/collision/2d/circle.h>
#include <sp2/collision/2d/polygon.h>
#include <sp2/collision/2d/box.h>
#include <sp2/collision/2d/ropejoint.h>
#include <sp2/script/environment.h>
#include <sp2/io/keybinding.h>
#include <sp2/audio/music.h>
#include <sp2/audio/sound.h>
#include <sp2/stringutil/convert.h>
#include <sp2/io/filesystem.h>
#include <nlohmann/json.hpp>
#include <optional>


class SaveProgressInterface {
public:
    virtual void save(nlohmann::json& json) = 0;
    virtual void load(nlohmann::json& json) = 0;
};

void saveGame()
{
    nlohmann::json json;
    for(auto node : sp::Scene::get("MAIN")->getRoot()->getChildren()) {
        auto spi = dynamic_cast<SaveProgressInterface*>(*node);
        if (spi) spi->save(json);
    }
    sp::io::saveFileContents(sp::io::preferencePath() + "progress.save", json.dump());
}

void createWorld();

sp::P<sp::Camera> camera;
sp::Vector2d start_position;
sp::Vector2d plane_start_position;
enum class IntroState {
    WaitForInitialStart,
    CrashingDown,
    Crashed,
    Done,
};
IntroState intro_state = IntroState::WaitForInitialStart;

sp::P<sp::gui::Widget> visible_message;
std::function<void()> post_message_function;

sp::InfiniGrid<bool> watermap{false};
sp::InfiniGrid<bool> mossmap{false};
std::unordered_map<sp::string, sp::P<sp::Tilemap>> tilemap_by_name;
std::unordered_map<sp::string, sp::Vector2d> secret_target;

sp::io::Keybinding key_up{"UP", {"up", "keypad 8", "w", "gamecontroller:0:button:dpup", "gamecontroller:0:axis:lefty"}};
sp::io::Keybinding key_down{"DOWN", {"down", "keypad 2", "s", "gamecontroller:0:button:dpdown"}};
sp::io::Keybinding key_left{"LEFT", {"left", "keypad 4", "a", "gamecontroller:0:button:dpleft"}};
sp::io::Keybinding key_right{"RIGHT", {"right", "keypad 6", "d", "gamecontroller:0:button:dpright", "gamecontroller:0:axis:leftx"}};

sp::io::Keybinding key_jump{"JUMP", {"space", "z", "gamecontroller:0:button:a"}};
sp::io::Keybinding key_menu{"MENU", {"escape", "gamecontroller:0:button:start"}};

void showMessage(sp::string message, std::function<void()> func={})
{
    visible_message = sp::gui::Loader::load("gui/msgbox.gui", "MSGBOX");
    visible_message->getWidgetWithID("MSG")->setAttribute("caption", message);
    sp::Engine::getInstance()->setGameSpeed(0.0);
    post_message_function = func;
}

class FadeLabel : public sp::gui::Label
{
public:
    FadeLabel(sp::P<sp::gui::Widget> parent) : sp::gui::Label(parent) {
        render_data.color.a = 0;
        timer.start(0.1);
    }

    void onUpdate(float delta) override {
        sp::gui::Label::onUpdate(delta);
        switch(state) {
        case State::WaitInitial:
            render_data.color.a = 0;
            if (timer.isExpired()) { state = State::FadeIn; timer.start(2.5); }
            break;
        case State::FadeIn:
            render_data.color.a = timer.getProgress();
            if (timer.isExpired()) { state = State::Wait; timer.start(2.5); }
            break;
        case State::Wait:
            if (timer.isExpired()) { state = State::FadeOut; timer.start(2.5); }
            break;
        case State::FadeOut:
            render_data.color.a = 1.0 - timer.getProgress();
            if (timer.isExpired()) {
                delete this;
            }
            break;
        }
    }

    void setAttribute(const sp::string& key, const sp::string& value) override
    {
        if (key == "delay") {
            timer.start(sp::stringutil::convert::toFloat(value));
        } else {
            sp::gui::Label::setAttribute(key, value);
        }
    }

    enum class State {
        WaitInitial,
        FadeIn,
        Wait,
        FadeOut,
    } state = State::WaitInitial;
    sp::Timer timer;
};
SP_REGISTER_WIDGET("fadelabel", FadeLabel);
class FadeOverlay : public sp::gui::Panel
{
public:
    FadeOverlay(sp::P<sp::gui::Widget> parent) : sp::gui::Panel(parent) {
        loadThemeStyle("overlay");
    }

    void onUpdate(float delta) override {
        sp::gui::Panel::onUpdate(delta);
        render_data.color.a = timer.getProgress();
    }

    void setAttribute(const sp::string& key, const sp::string& value) override
    {
        if (key == "delay") {
            timer.start(sp::stringutil::convert::toFloat(value));
        } else {
            sp::gui::Panel::setAttribute(key, value);
        }
    }
    sp::Timer timer;
};
SP_REGISTER_WIDGET("fadeoverlay", FadeOverlay);

/*
std::optional<sp::Vector2d> traceCollision(const sp::Ray2d& ray)
{
    auto delta = ray.end - ray.start;

    auto dirSignX = delta.x >= 0 ? 1 : -1;
    auto dirSignY = delta.y >= 0 ? 1 : -1;
    auto tileOffsetX = delta.x >= 0 ? 1 : 0;
    auto tileOffsetY = delta.y >= 0 ? 1 : 0;

    auto p = ray.start;
    auto tile = sp::Vector2i(std::floor(p.x), std::floor(p.y));
    auto t = 0.0;

    if (collision_map.get(tile))
        return ray.start;
    if (ray.start == ray.end)
        return {};

    while(t < 1.0) {
        if (collision_map.get(tile))
            return p;
        auto dtx = (tile.x - p.x + tileOffsetX) / delta.x;
        auto dty = (tile.y - p.y + tileOffsetY) / delta.y;
        if (dtx < dty) {
            t += dtx;
            tile.x += dirSignX;
        } else {
            t += dty;
            tile.y += dirSignY;
        }
        p = ray.start + delta * t;
    }

    return {};
}
*/

class Checkpoint : public sp::Node, public SaveProgressInterface
{
public:
    Checkpoint(sp::P<sp::Node> parent)
    : sp::Node(parent)
    {
        setAnimation(sp::SpriteAnimation::load("flag.txt"));
        animationPlay("Idle");

        sp::collision::Box2D shape{0.5, 0.5};
        shape.type = sp::collision::Shape::Type::Sensor;
        setCollisionShape(shape);
    }

    void activate()
    {
        if (!is_checked) sp::audio::Sound::play("sfx/checkpoint.wav");
        is_checked = true;
        animationPlay("Active");
    }

    void check()
    {
        if (!is_checked) sp::audio::Sound::play("sfx/checkpoint.wav");
        is_checked = true;
        animationPlay("Found");
    }

    sp::P<Checkpoint> teleport(double direction) {
        sp::P<Checkpoint> best_checkpoint;
        double best_distance = 100000.0;
        double best_angle = 90;
        for(auto node : getParent()->getChildren()) {
            sp::P<Checkpoint> checkpoint = node;
            if (checkpoint && checkpoint != this && checkpoint->is_checked) {
                auto offset = checkpoint->getPosition2D() - getPosition2D();
                auto distance = offset.length();
                auto angle = std::abs(sp::angleDifference(direction, offset.angle()));
                if (angle > 45) continue;
                if (distance + angle * 0.1 > best_distance + best_angle * 0.1) continue;
                best_checkpoint = checkpoint;
                best_distance = distance;
                best_angle = angle;
            }
        }
        return best_checkpoint;
    }

    bool is_checked = false;
    int id = -1;

    void save(nlohmann::json& json) override {
        if (is_checked) json["checkpoint_" + std::to_string(id)] = true;
    }
    void load(nlohmann::json& json) override {
        auto it = json.find("checkpoint_" + std::to_string(id));
        if (it != json.end()) is_checked = *it;
        if (is_checked) check();
    }
};

class FadeOutNode : public sp::Node
{
public:
    FadeOutNode(sp::P<sp::Node> parent)
    : sp::Node(parent) {
        timer.start(0.5);
    }

    void onUpdate(float delta) override {
        render_data.color.a = 1.0 - timer.getProgress();
        if (timer.isExpired())
            delete this;
    }

    sp::Timer timer;
};

class Player : public sp::Node, public SaveProgressInterface
{
public:
    Player(sp::P<sp::Node> parent)
    : sp::Node(parent)
    {
        setAnimation(sp::SpriteAnimation::load("player.txt"));
        animationPlay("Idle");

        sp::collision::Box2D shape{0.4, 0.8};
        shape.type = sp::collision::Shape::Type::Dynamic;
        shape.fixed_rotation = true;
        setCollisionShape(shape);

        death_line = new sp::Node(getParent());
        death_line->render_data.shader = sp::Shader::get("internal:basic.shader");
        sp::MeshBuilder mb;
        for(int n=-30; n<30; n++)
            mb.addQuad({n - 0.5f, -0.5f, 0}, {n + 0.5f, -0.5f, 0}, {n - 0.5f, 0.5f, 0}, {n + 0.5f, 0.5f, 0});
        death_line->render_data.mesh = mb.create();
        death_line->render_data.texture = sp::texture_manager.get("danger-line.png");
        death_line->render_data.order = 1000;
        death_line->setPosition(sp::Vector2d(0, -10000));
    }

    void onUpdate(float delta) override
    {
        if (camera_shake.isExpired() || camera_shake.isRunning()) {
            camera->setPosition(camera->getPosition2D() + sp::Vector2d(sp::random(-0.1, 0.1), sp::random(-0.1, 0.1)));
        }

        if (visible_message) {
            if (key_jump.getDown()) {
                if (state == State::Walking) state = State::Falling;
                visible_message.destroy();
                sp::Engine::getInstance()->setGameSpeed(1.0f);
                if (post_message_function) {
                    auto f = post_message_function;
                    f();
                }
            }
            return;
        }
        if (first_death_timer.isExpired()) {
            showMessage("Wow, I should be more careful.", [](){
                showMessage("Falling beyond a certain\ndistance could be painful");
            });
        }

        auto target_y = death_height - 0.9;
        auto delta_y = target_y - death_line->getPosition2D().y;
        death_line->setPosition({std::floor(getPosition2D().x), death_line->getPosition2D().y + delta_y * 0.1});

        if (state == State::Death) return;
        auto pos = getPosition2D();
        auto camera_pos = camera->getPosition2D();
        camera_pos.x = pos.x;
        if (camera_pos.y > pos.y || state == State::Walking || state == State::Hanging || state == State::ClimbUp || state == State::Teleport || (state == State::Swimming && getLinearVelocity2D().y > -3)) {
            auto y_delta = pos.y - camera_pos.y;
            camera_pos.y += y_delta * delta * 3.0;
        }
        camera->setPosition(camera_pos);
    }

    void onFixedUpdate() override
    {
        auto jump_velocity = 9.0;
        auto gravity = 20.0;
        auto jump_gravity = 15.0;
        auto move_speed = 4.0;
        auto jump_max_v = 5.5;

        auto velocity = getLinearVelocity2D();

        bool old_in_water = in_water;
        in_water = watermap.get({int(std::floor(getPosition2D().x)), int(std::floor(getPosition2D().y + 0.25))});
        if (in_water != old_in_water && in_water) {
            sp::audio::Sound::play("sfx/water.wav");
            auto pe = new sp::ParticleEmitter(getParent(), "splash.particles.txt");
            pe->setPosition(getPosition2D() + sp::Vector2d(0, -0.2));
        }

        if (state == State::Jumping) {
            if (velocity.y <= jump_max_v)
                state = State::Falling;
            if (!key_jump.get()) {
                velocity.y *= 0.3;
                state = State::Falling;
            }
        }
        if (state == State::Walking) {
            to_fall_state_delay -= 1;
            if (to_fall_state_delay <= 0)
                state = State::Falling;
        }
        if ((state == State::Falling || state == State::Walking) && in_water) {
            state = State::Swimming;
        }
        if (state == State::Swimming && !in_water) {
            state = State::Falling;
        }

        switch(state)
        {
        case State::Teleport: {
            }break;
        case State::Hanging:
            if (!checkCollisionHorizontal({inFaceDir(1), 0.25})) {
                state = State::Falling;
            }
            break;
        case State::ClimbUp:
            if (!checkCollisionHorizontal({inFaceDir(1), -0.4})) {
                state = State::Falling;
                velocity.y = 0.0;
                setPosition(getPosition2D() + sp::Vector2d(inFaceDir(0.1), 0));
                updateFallDepth();
            } else {
                velocity.y = 4.0;
            }
            break;
        case State::Swimming:
            velocity.y *= 0.9;
            if (watermap.get({int(std::floor(getPosition2D().x)), int(std::floor(getPosition2D().y + 0.35))})) {
                velocity.y += 10.0 * sp::Engine::fixed_update_delta;
                if (can_dive) {
                    auto request = key_up.getValue() - key_down.getValue();
                    velocity.y += request * 20.0 * sp::Engine::fixed_update_delta;
                    if (std::abs(velocity.y) < 3)
                        updateFallDepth();
                }
            } else {
                velocity.y -= 0.1 * sp::Engine::fixed_update_delta;
                if (can_dive) {
                    auto request = -key_down.getValue();
                    velocity.y += request * 20.0 * sp::Engine::fixed_update_delta;
                }
                updateFallDepth();
            }
            break;
        case State::Jumping:
            velocity.y -= jump_gravity * sp::Engine::fixed_update_delta;
            break;
        default:
            velocity.y -= gravity * sp::Engine::fixed_update_delta;
            break;
        }
        if (wall_jump_time > 0) {
            wall_jump_time --;
        } else if (state == State::Teleport) {
            velocity.x = 0.0;
        } else if (state == State::Hanging || state == State::ClimbUp) {
            velocity.x = 0.0;
        } else if (state == State::Swinging) {
            auto request = key_right.getValue() - key_left.getValue();
            velocity.x *= 0.97;
            velocity.x += request * 0.2;
            int index = 0;
            for(auto rn : rope_nodes) {
                rn->setPosition(sp::Tween<sp::Vector2d>::linear(0.2f + 0.2f * index, 0.0f, 1.0f, getPosition2D(), rope_attachpoint));
                index++;
            }
        } else if (state != State::Death) {
            auto target_velocity = (key_right.getValue() - key_left.getValue()) * move_speed;
            auto delta = target_velocity - velocity.x;
            if (std::abs(delta) <= move_speed) {
                velocity.x = target_velocity;
            } else {
                velocity.x += std::copysign(move_speed, delta) * 0.3;
            }
        }
        if (key_jump.getDown()) {
            if (state == State::Walking || state == State::Falling) {
                jump_buffer = 4;
            }
            if (state == State::Swimming && !watermap.get({int(std::floor(getPosition2D().x)), int(std::floor(getPosition2D().y + 0.4))})) {
                velocity.y += jump_velocity;
                state = State::Jumping;
                jump_count += 1;
            }
            if (state == State::Hanging) {
                if ((key_left.get() && (animationGetFlags() & sp::SpriteAnimation::FlipFlag)) || (key_right.get() && !(animationGetFlags() & sp::SpriteAnimation::FlipFlag))) {
                    state = State::ClimbUp;
                } else {
                    sp::audio::Sound::play("sfx/blip.wav");
                    double wall_jump_angle = 40.0;
                    velocity.y += jump_velocity * std::sin(wall_jump_angle / 180.0 * sp::pi);
                    velocity.x = -inFaceDir(jump_velocity * std::cos(wall_jump_angle / 180.0 * sp::pi));
                    state = State::Jumping;
                    wall_jump_time = 7;
                    jump_count += 1;
                }
            }
            if (state == State::Teleport) {
                state = State::Falling;
                for(auto n : teleport_arrows)
                    n.destroy();
            } else if (state == State::Falling && aboveDeathLine(-1.0) && can_rope) {
                rope_attachpoint = getPosition2D() + sp::Vector2d(inFaceDir(2.5), 2.5);
                if (!tryToRope(rope_attachpoint)) {
                    if (!tryToRope(rope_attachpoint - sp::Vector2d(0.2, 0)))
                        tryToRope(rope_attachpoint + sp::Vector2d(0.2, 0));
                }

                if (!rope_joint) {
                    for(int n=0; n<5; n++) {
                        auto rn = new FadeOutNode(getParent());
                        rn->render_data.shader = sp::Shader::get("internal:color.shader");
                        rn->render_data.mesh = sp::MeshData::createQuad({0.1, 0.1});
                        rn->render_data.type = sp::RenderData::Type::Normal;
                        rn->setPosition(sp::Tween<sp::Vector2d>::linear(0.2f + 0.2f * n, 0.0f, 1.0f, getPosition2D(), rope_attachpoint));
                    }
                }
            }
        }
        if (key_jump.getUp() && state == State::Swinging) {
            for(auto n : rope_nodes)
                n.destroy();
            rope_joint.destroy();
            state = State::Falling;
        }
        if (jump_buffer) {
            if (state == State::Walking) {
                velocity.y += jump_velocity;
                state = State::Jumping;
                sp::audio::Sound::play("sfx/blip.wav");
                jump_buffer = 0;
                jump_count += 1;
            } else {
                jump_buffer--;
            }
        }
        if (key_up.getDown()) {
            if (state == State::Hanging) {
                state = State::ClimbUp;
            } else if (state == State::Walking) {
                if (can_teleport && checkpoint && (checkpoint->getPosition2D() - getPosition2D()).length() < 1.0) {
                    tele_count += 1;
                    state = State::Teleport;
                    velocity.y = 0;
                    setPosition(checkpoint->getPosition2D() + sp::Vector2d(0, 0.7));
                    buildTeleArrows();
                }
            } else if (state == State::Teleport) {
                teleport(90);
            }
        }
        if (key_down.getDown()) {
            if (state == State::Hanging) {
                setPosition(getPosition2D() - sp::Vector2d(0, 0.1));
                state = State::Falling;
            }
            if (state == State::Teleport)
                teleport(-90);
        }
        if (key_left.getDown()) {
            if (state == State::Teleport)
                teleport(180);
        }
        if (key_right.getDown()) {
            if (state == State::Teleport)
                teleport(0);
        }
        setLinearVelocity(velocity);
        if (state == State::Death) {
            if (respawn_delay) {
                respawn_delay--;
            } else {
                respawn();
            }
        } else if (getPosition2D().y < death_height - 6.0) {
            sp::audio::Sound::play("sfx/death.wav");
            state = State::Death;
            respawn_delay = 30;
            camera_shake.start(0.3);
        }

        if (state == State::Walking && velocity.x != 0) {
            animationPlay("Walk");
            animationSetFlags(velocity.x < 0 ? sp::SpriteAnimation::FlipFlag : 0);
        } else if (state == State::Swimming) {
            animationPlay("Swim");
            if (velocity.x != 0)
                animationSetFlags(velocity.x < 0 ? sp::SpriteAnimation::FlipFlag : 0);
        } else if (state == State::Hanging) {
            animationPlay("Hang");
        } else if (state == State::ClimbUp) {
            animationPlay("ClimbUp");
        } else if (state == State::Swinging) {
            animationPlay("Swing");
            if (velocity.x < -1)
                animationSetFlags(sp::SpriteAnimation::FlipFlag);
            else if (velocity.x > 1)
                animationSetFlags(0);
        } else if (state == State::Jumping || state == State::Falling) {
            animationPlay("Jump");
            if (velocity.x < 0)
                animationSetFlags(sp::SpriteAnimation::FlipFlag);
            else if (velocity.x > 0)
                animationSetFlags(0);
        } else if (state == State::Death) {
            animationPlay("Dead");
        } else if (state == State::Teleport) {
            animationPlay("Teleport");
        } else {
            animationPlay("Idle");
        }
        if (key_menu.getDown()) {
            auto gui = sp::gui::Loader::load("gui/ingame.gui", "MENU");
            gui->getWidgetWithID("RESUME")->setEventCallback([=](sp::Variant) mutable {
                gui.destroy();
                sp::Engine::getInstance()->setPause(false);
            });
            gui->getWidgetWithID("RESET")->setEventCallback([=](sp::Variant) mutable {
                gui.destroy();
                sp::audio::Music::stop();
                sp::Engine::getInstance()->setPause(false);
                sp::io::saveFileContents(sp::io::preferencePath() + "progress.save", "");
                sp::Scene::get("MAIN").destroy();
                intro_state = IntroState::WaitForInitialStart;
                createWorld();
            });
            gui->getWidgetWithID("QUIT")->setEventCallback([](sp::Variant) {
                sp::Engine::getInstance()->shutdown();
            });
#ifdef EMSCRIPTEN
            gui->getWidgetWithID("QUIT")->hide();
#endif
            sp::Engine::getInstance()->setPause(true);
        }
    }

    bool tryToRope(sp::Vector2d target) {
        getScene()->queryCollisionAll({getPosition2D(), target}, [&](sp::P<sp::Node> node, sp::Vector2d hit_location, sp::Vector2d hit_normal) {
            if (node->isSolid()) {
                rope_attachpoint = hit_location;
                if (sp::P<sp::Tilemap>(node) && hit_normal.y < -0.5 && mossmap.get({int(std::floor(hit_location.x)), int(std::floor(hit_location.y))})) {
                    state = State::Swinging;
                    sp::audio::Sound::play("sfx/rope.wav");
                    rope_joint = new sp::collision::RopeJoint2D(this, {0, 0}, node, hit_location, (getPosition2D() - hit_location).length());
                    for(int n=0; n<5; n++) {
                        auto rn = new sp::Node(getParent());
                        rn->render_data.shader = sp::Shader::get("internal:color.shader");
                        rn->render_data.mesh = sp::MeshData::createQuad({0.1, 0.1});
                        rn->render_data.type = sp::RenderData::Type::Normal;
                        rn->setPosition(sp::Tween<sp::Vector2d>::linear(0.2f + 0.2f * n, 0.0f, 1.0f, getPosition2D(), hit_location));
                        rope_nodes.add(rn);
                    }
                }
                return false;
            }
            return true;
        });
        return rope_joint != nullptr;
    }

    void onCollision(sp::CollisionInfo& info) override
    {
        sp::P<Checkpoint> cp = info.other;
        if (cp && (state == State::Walking || state == State::Swimming) && cp != checkpoint) {
            setCheckpoint(cp);
        }
        if (info.other && info.other->isSolid()) {
            if (info.normal.y < -0.5) { // Hit floor
                if (!aboveDeathLine()) {
                    kill();
                } else if (state != State::Death && state != State::Teleport && state != State::Jumping) {
                    updateFallDepth();
                    state = State::Walking;
                    for(auto n : rope_nodes)
                        n.destroy();
                    rope_joint.destroy();
                    to_fall_state_delay = coyote_time;
                }
            } else if (info.normal.y > 0.5) { // Hit ceiling
                if (state == State::Jumping)
                    state = State::Falling;
            } else if (std::abs(info.normal.x) > 0.5) { // Hit wall
                if (getLinearVelocity2D().y < 0.0 && aboveDeathLine() && can_hang) {
                    if (!checkCollisionHorizontal({std::copysign(1, info.normal.x), 0.4}) &&
                        checkCollisionHorizontal({std::copysign(1, info.normal.x), 0.25})) {
                        auto vertical_hit = checkCollisionVertical({std::copysign(1, info.normal.x), 0.4});
                        if (vertical_hit) {
                            state = State::Hanging;
                            for(auto n : rope_nodes)
                                n.destroy();
                            rope_joint.destroy();
                            setLinearVelocity(sp::Vector2d(0, 0));
                            setPosition({getPosition2D().x, vertical_hit.value().y - 0.3});
                            animationSetFlags((info.normal.x < 0) ? sp::SpriteAnimation::FlipFlag : 0);
                            updateFallDepth();
                        }
                    }
                }
            }
        }
    }

    void teleport(double direction) {
        if (checkpoint) {
            auto target = checkpoint->teleport(direction);
            if (target) {
                setCheckpoint(target);
                setPosition(target->getPosition2D() + sp::Vector2d(0, 0.7));
                updateFallDepth();
                sp::audio::Sound::play("sfx/tele.wav");
                buildTeleArrows();
            }
        }
    }

    void buildTeleArrows() {
        for(auto n : teleport_arrows)
            n.destroy();
        if (checkpoint) {
            for(double dir : {0.0, 90.0, 180.0, 270.0}) {
                if (auto target = checkpoint->teleport(dir)) {
                    auto target_dir = (target->getPosition2D() - checkpoint->getPosition2D()).angle();
                    dir += sp::angleDifference(dir, target_dir) * 0.7;
                    auto n = new sp::Node(this);
                    n->setPosition(sp::Vector2d(0.5, 0).rotate(dir));
                    n->setRotation(dir + 180.0);
                    n->render_data.shader = sp::Shader::get("internal:basic.shader");
                    n->render_data.mesh = sp::MeshData::createQuad({1, 1});
                    n->render_data.type = sp::RenderData::Type::Normal;
                    n->render_data.texture = sp::texture_manager.get("arrow.png");
                    n->render_data.order = 1000;
                    teleport_arrows.add(n);
                }
            }
        }
    }

    void setCheckpoint(sp::P<Checkpoint> cp) {
        if (cp == checkpoint) return;
        if (checkpoint)
            checkpoint->check();
        checkpoint = cp;
        checkpoint->activate();
        saveGame();
    }

    void kill()
    {
        if (state != State::Death) {
            for(auto n : rope_nodes)
                n.destroy();
            rope_joint.destroy();
            sp::audio::Sound::play("sfx/death.wav");
            state = State::Death;
            respawn_delay = 30;
            camera_shake.start(0.3);
            setLinearVelocity({0, 0});
            auto pe = new sp::ParticleEmitter(getParent(), "death.particles.txt");
            pe->setPosition(getPosition2D());
        }
    }

    void respawn()
    {
        if (checkpoint)
            setPosition(checkpoint->getPosition2D());
        else
            setPosition(start_position);
        state = State::Falling;
        
        updateFallDepth();

        if (death_count == 0) {
            first_death_timer.start(0.5);
        }
        death_count++;
    }

    bool aboveDeathLine(double offset = 0) {
        return getPosition2D().y >= death_height + offset;
    }

    double inFaceDir(double value) {
        return (animationGetFlags() & sp::SpriteAnimation::FlipFlag) ? -value : value;
    }

    bool checkCollisionHorizontal(sp::Vector2d offset)
    {
        bool hit_solid = false;
        getScene()->queryCollisionAny({getPosition2D() + sp::Vector2d(0, offset.y), getPosition2D() + offset}, [&hit_solid](sp::P<sp::Node> node, sp::Vector2d hit_location, sp::Vector2d hit_normal) {
            if (node->isSolid())
                hit_solid = true;
            return !hit_solid;
        });
        return hit_solid;
    }

    std::optional<sp::Vector2d> checkCollisionVertical(sp::Vector2d offset)
    {
        std::optional<sp::Vector2d> result;
        getScene()->queryCollisionAll({getPosition2D() + offset, getPosition2D() + sp::Vector2d(offset.x, -offset.y)}, [&result](sp::P<sp::Node> node, sp::Vector2d hit_location, sp::Vector2d hit_normal) {
            if (node->isSolid())
                result = hit_location;
            return !result.has_value();
        });
        return result;
    }

    void updateFallDepth()
    {
        auto max_fall_depth = 4.5;
        death_height = getPosition2D().y - max_fall_depth;
    }

    void save(nlohmann::json& json) override {
        if (checkpoint) json["current_checkpoint"] = checkpoint->id;
        json["death_count"] = death_count;
        json["tele_count"] = tele_count;
        json["jump_count"] = jump_count;
        if (death_line->render_data.type == sp::RenderData::Type::Normal) json["death_line"] = true;
        if (can_hang) json["can_hang"] = true;
        if (can_teleport) json["can_teleport"] = true;
        if (can_dive) json["can_dive"] = true;
        if (can_rope) json["can_rope"] = true;
    }
    void load(nlohmann::json& json) override {
        auto it = json.find("current_checkpoint");
        if (it != json.end()) {
            int checkpoint_id = *it;
            for(sp::P<Checkpoint> cp : getParent()->getChildren()) {
                if (cp && cp->id == checkpoint_id)
                    checkpoint = cp;
            }
        }
        it = json.find("death_line");
        if (it != json.end() && bool(*it)) death_line->render_data.type = sp::RenderData::Type::Normal;
        it = json.find("death_count");
        if (it != json.end()) death_count = *it;
        it = json.find("tele_count");
        if (it != json.end()) tele_count = *it;
        it = json.find("jump_count");
        if (it != json.end()) jump_count = *it;
        it = json.find("can_hang");
        if (it != json.end()) can_hang = *it;
        it = json.find("can_teleport");
        if (it != json.end()) can_teleport = *it;
        it = json.find("can_dive");
        if (it != json.end()) can_dive = *it;
        it = json.find("can_rope");
        if (it != json.end()) can_rope = *it;
    }

    sp::Vector2d velocity;
    double death_height = -10000;
    int death_count = 0;
    int tele_count = 0;
    int jump_count = 0;
    sp::Timer first_death_timer;
    sp::P<sp::Node> death_line;
    bool in_water = false;
    enum class State {
        Walking,
        Jumping,
        Falling,
        Swimming,
        Hanging,
        ClimbUp,
        Death,
        Teleport,
        Swinging,
    } state = State::Falling;
    int to_fall_state_delay = 0;
    int jump_buffer = 0;
    int wall_jump_time = 0;
    static constexpr int coyote_time = 7;
    sp::P<Checkpoint> checkpoint;
    int respawn_delay = 0;
    sp::Vector2d rope_attachpoint;
    sp::P<sp::collision::RopeJoint2D> rope_joint;
    sp::PList<sp::Node> rope_nodes;
    sp::PList<sp::Node> teleport_arrows;

    bool can_hang = false;
    bool can_teleport = false;
    bool can_dive = false;
    bool can_rope = false;
    sp::Timer camera_shake;
};
sp::P<Player> player;

class Pickup : public sp::Node, public SaveProgressInterface
{
public:
    enum class Type {
        TapeMeasure,
        ClimbingGlove,
        Teleport,
        DivingHelmet,
        RadioactiveSpider,
    };
    Pickup(sp::P<sp::Node> parent, sp::Vector2d position, Type type, int id)
    : sp::Node(parent), type(type), position(position), id(id)
    {
        render_data.shader = sp::Shader::get("internal:basic.shader");
        render_data.mesh = sp::MeshData::createQuad({1, 1});
        render_data.type = sp::RenderData::Type::Normal;
        switch(type)
        {
        case Type::TapeMeasure: render_data.texture = sp::texture_manager.get("tapemeasure.png"); break;
        case Type::ClimbingGlove: render_data.texture = sp::texture_manager.get("climbingglove.png"); break;
        case Type::Teleport: render_data.texture = sp::texture_manager.get("teleport.png"); break;
        case Type::DivingHelmet: render_data.texture = sp::texture_manager.get("diving.png"); break;
        case Type::RadioactiveSpider: render_data.texture = sp::texture_manager.get("rspider.png"); break;
        }
        setPosition(position);
        emitters.add(new sp::ParticleEmitter(getParent(), "pickup.particles.a.txt"));
        emitters.add(new sp::ParticleEmitter(getParent(), "pickup.particles.b.txt"));
        for(auto e : emitters) {
            e->setPosition(position);
            e->render_data.order = -1;
        }
        sp::collision::Box2D shape{0.5, 0.5};
        shape.type = sp::collision::Shape::Type::Sensor;
        setCollisionShape(shape);
    }

    virtual void onUpdate(float delta) override
    {
    }

    void onCollision(sp::CollisionInfo& info) override
    {
        if (info.other != player) return;
        sp::audio::Sound::play("sfx/pickup.wav");
        switch(type) {
        case Type::TapeMeasure:
            showMessage("Found the tapemeasure!", [](){
                showMessage("Now you can measure how far\nyou can fall before you die", []() {
                    player->death_line->render_data.type = sp::RenderData::Type::Normal;
                    saveGame();
                });
            });
            break;
        case Type::ClimbingGlove:
            showMessage("Found the climbing glove!", [](){
                showMessage("You can now hang on\nthe edges of cliffs", []() {
                    showMessage("Press the UP key to\nclimb up when hanging", []() {
                        player->can_hang = true;
                        saveGame();
                    });
                });
            });
            break;
        case Type::Teleport:
            showMessage("Found the magic hat!", [](){
                showMessage("Hold UP on flags\nto teleport", []() {
                    player->can_teleport = true;
                    saveGame();
                });
            });
            break;
        case Type::DivingHelmet:
            showMessage("Found the diving helmet!", [](){
                showMessage("You can now swim\nunder water", []() {
                    showMessage("Comes with unlimited air\n(don't question it)", []() {
                        player->can_dive = true;
                        saveGame();
                    });
                });
            });
            break;
        case Type::RadioactiveSpider:
            showMessage("Found a radioactive spider!", [](){
                showMessage("You suddenly shoot\nwebs out of your wrists", []() {
                    showMessage("Press and hold jump\nwhile jumping to swing!", []() {
                        player->can_rope = true;
                        saveGame();
                    });
                });
            });
            break;
        }
        (new sp::ParticleEmitter(getParent(), "pickup.done.particles.a.txt"))->setPosition(getPosition2D());
        (new sp::ParticleEmitter(getParent(), "pickup.done.particles.b.txt"))->setPosition(getPosition2D());
        hide();
    }

    void hide()
    {
        for(auto e : emitters) {
            e->stopSpawn();
            e->auto_destroy = true;
        }
        
        removeCollisionShape();
        render_data.type = sp::RenderData::Type::None;
    }

    void save(nlohmann::json& json) override {
        if (render_data.type == sp::RenderData::Type::None) json["pickup_" + std::to_string(id)] = true;
    }
    void load(nlohmann::json& json) override {
        auto it = json.find("pickup_" + std::to_string(id));
        if (it != json.end() && bool(*it)) hide();
    }

    Type type;
    sp::Vector2d position;
    sp::PList<sp::ParticleEmitter> emitters;
    int id;
};

class IntroCloud : public sp::Node
{
public:
    IntroCloud(sp::P<sp::Node> parent)
    : sp::Node(parent)
    {
        render_data.shader = sp::Shader::get("internal:color.shader");
        render_data.mesh = sp::MeshData::createQuad({sp::random(0.5, 1.0), sp::random(0.25, 0.5)});
        render_data.type = sp::RenderData::Type::Normal;
        velocity = sp::random(-2, 2);
    }

    void onUpdate(float delta) override {
        setPosition(getPosition2D() + (plane_start_position - start_position).normalized() * double(delta * velocity));
        switch(intro_state)
        {
        case IntroState::WaitForInitialStart:
            setPosition(getPosition2D() + (plane_start_position - start_position).normalized() * double(delta * 20.0));
            if (getPosition2D().x < plane_start_position.x - 20)
                delete this;
            break;
        case IntroState::CrashingDown:
            break;
        case IntroState::Crashed:
        case IntroState::Done:
            render_data.color.a -= delta;
            if (render_data.color.a <= 0.0)
                delete this;
            break;
        }
    }

    float anim_time = 0.0;
    double velocity = 0;
};

class Plane : public sp::Node
{
public:
    Plane(sp::P<sp::Node> parent)
    : sp::Node(parent)
    {
        render_data.shader = sp::Shader::get("internal:basic.shader");
        render_data.mesh = sp::MeshData::createQuad({3, 2});
        render_data.type = sp::RenderData::Type::Normal;
        render_data.texture = sp::texture_manager.get("plane.png");
        //setAnimation(sp::SpriteAnimation::load("player.txt"));
        //animationPlay("Idle");
        for(int n=0; n<60; n++) {
            auto cloud = new IntroCloud(getParent());
            cloud->setPosition(plane_start_position + sp::Vector2d(sp::random(-20, 20), sp::random(-20, 20)).rotate(getRotation2D()));
        }

        engine_emitter = new sp::ParticleEmitter(this, "plane.engine.particles.txt");
        engine_emitter->setPosition({-0.5, 0});
    }

    void onUpdate(float delta) override {
        switch(intro_state) {
        case IntroState::WaitForInitialStart: {
            anim_time += delta;
            auto anim_y = std::sin(anim_time) * 0.5 + std::sin(anim_time / 3.15) * 0.25 + std::cos(anim_time * 5.15) * 0.1;
            setPosition(plane_start_position + sp::Vector2d(0, anim_y).rotate(getRotation2D()));

            auto cloud = new IntroCloud(getParent());
            cloud->setPosition(plane_start_position + sp::Vector2d(20, sp::random(-20, 20)).rotate(getRotation2D()));
            if (key_jump.getDown() || key_menu.getDown()) {
                intro_state = IntroState::CrashingDown;
                plane_start_position = getPosition2D();
                anim_time = 0;
            }
            }break;
        case IntroState::CrashingDown:{
            setPosition(getPosition2D() + (start_position - plane_start_position).normalized() * double(delta * 20.0));
            camera->setPosition(camera->getPosition2D() + (start_position - plane_start_position).normalized() * double(delta * 20.0));
            if (getPosition2D().y < start_position.y) {
                setPosition({getPosition2D().x, start_position.y});
                intro_state = IntroState::Crashed;
                player = new Player(getScene()->getRoot());
                player->setPosition(start_position);
                player->camera_shake.start(0.4);
                engine_emitter->stopSpawn();
                engine_emitter->auto_destroy = true;

                auto pe = new sp::ParticleEmitter(this, "plane.engine.particles.b.txt");
                pe->setPosition({-0.8, 0});
                pe->setRotation(-getRotation2D());

                sp::audio::Sound::play("sfx/explosion.wav");
                auto ee = new sp::ParticleEmitter(getParent(), "plane.explosion.particles.a.txt");
                ee->setPosition(getPosition2D());
                ee = new sp::ParticleEmitter(getParent(), "plane.explosion.particles.b.txt");
                ee->setPosition(getPosition2D());
            }
            }break;
        case IntroState::Crashed:{
            if (!engine_emitter) {
                showMessage("... auch ...", []() {
                    showMessage("I seem to have crashed\non the top of this mountain.", []() {
                        showMessage("I better try to get down.", []() {
                            sp::gui::Loader::load("gui/title.gui", "TITLE");
                            sp::audio::Music::play("music/A Tale of Wind - MP3.ogg");
                        });
                    });
                });
                intro_state = IntroState::Done;
            }
            }break;
        case IntroState::Done:
            break;
        }
    }

    sp::P<sp::ParticleEmitter> engine_emitter;
    float anim_time = 0.0;
};

class HideLayerTrigger : public sp::Node
{
public:
    HideLayerTrigger(sp::P<sp::Node> parent, sp::Rect2d area)
    : sp::Node(parent), area(area)
    {
    }

    void onUpdate(float delta) override
    {
        if (player && area.contains(player->getPosition2D())) {
            getParent()->render_data.color.a = std::max(0.0f, getParent()->render_data.color.a - delta);
        } else {
            getParent()->render_data.color.a = std::min(1.0f, getParent()->render_data.color.a + delta);
        }
    }

    sp::Rect2d area;
};

class KillZone : public sp::Node
{
public:
    KillZone(sp::P<sp::Node> parent, sp::Vector2d position, sp::Vector2d size)
    : sp::Node(parent)
    {
        setPosition(position);
        sp::collision::Box2D shape{size.x, size.y};
        shape.type = sp::collision::Shape::Type::Sensor;
        setCollisionShape(shape);
    }

    void onCollision(sp::CollisionInfo& info) override
    {
        if (info.other != player) return;
        player->kill();
    }
};

class FallingBlock : public sp::Node
{
public:
    FallingBlock(sp::P<sp::Node> parent, sp::Vector2d position)
    : sp::Node(parent), position(position)
    {
        render_data.shader = sp::Shader::get("internal:basic.shader");
        render_data.mesh = sp::MeshData::createQuad({2, 1});
        render_data.type = sp::RenderData::Type::Normal;
        render_data.texture = sp::texture_manager.get("fallingblock2x1.png");
        render_data.order = -1;

        setPosition(position);
        sp::collision::Box2D shape{2.0, 1.0};
        shape.type = sp::collision::Shape::Type::Kinematic;
        setCollisionShape(shape);
    }

    void onFixedUpdate() override
    {
        switch(state) {
        case State::Idle: break;
        case State::Triggered:
            if (state_timer.isExpired()) {
                state = State::Falling;
                state_timer.start(2.5);
                sp::audio::Sound::play("sfx/breakblock.wav");
            }
            break;
        case State::Falling:
            setLinearVelocity(getLinearVelocity2D() - sp::Vector2d(0, 1));
            if (getLinearVelocity2D().y == -10) {
                sp::collision::Box2D shape{2.0, 1.0};
                shape.type = sp::collision::Shape::Type::Sensor;
                setCollisionShape(shape);
            }
            if (state_timer.isExpired()) {
                setPosition(position);
                setLinearVelocity({0, 0});
                state = State::Idle;

                sp::collision::Box2D shape{2.0, 1.0};
                shape.type = sp::collision::Shape::Type::Kinematic;
                setCollisionShape(shape);
            }
            break;
        }
    }

    void onCollision(sp::CollisionInfo& info) override
    {
        if (info.other != player) return;
        if (state == State::Idle) {
            state = State::Triggered;
            state_timer.start(0.8);
        }
    }

    sp::Vector2d position;
    enum class State {
        Idle,
        Triggered,
        Falling,
    } state = State::Idle;
    sp::Timer state_timer;
};

class MessageSignTrigger : public sp::Node
{
public:
    MessageSignTrigger(sp::P<sp::Node> parent)
    : sp::Node(parent)
    {
    }

    void onUpdate(float delta) override
    {
        if (player && player->state == Player::State::Walking && (player->getPosition2D() - getPosition2D()).length() < 1.0) {
            if (!popup_message) {
                popup_message = sp::gui::Loader::load("gui/msgbox.gui", "MSGBOX");
                if (secret) {
                    popup_message->getWidgetWithID("MSG")->setAttribute("style", "secret");
                    popup_message->getWidgetWithID("MSG")->setAttribute("text.alignment", "center");
                    decode_message = message.format([](const sp::string& key) {
                        int number = sp::stringutil::convert::toInt(key);
                        if (key == "D") number = player->death_count;
                        if (key == "T") number = player->tele_count;
                        if (key == "J") number = player->jump_count;
                        sp::string result;
                        while(number > 0) {
                            int digit = number % 15;
                            if (digit < 10) result = char('0' + digit) + result;
                            else result = char('a' + digit - 10) + result;
                            number /= 15;
                        }
                        if (result.size() == 0) result = "0";
                        return result;
                    });
                } else {
                    decode_message = message;
                }
            }
            msgsize += delta * 30.0f;
            if (msgsize > decode_message.length())
                msgsize = decode_message.length();
            popup_message->getWidgetWithID("MSG")->setAttribute("caption", decode_message.substr(0, int(msgsize)));
        } else {
            if (popup_message) {
                msgsize -= delta * 100.0f;
                popup_message->getWidgetWithID("MSG")->setAttribute("caption", decode_message.substr(0, int(msgsize)));
                if (msgsize < 1.0)
                    popup_message.destroy();
            }
        }
    }

    float msgsize = 0.0;
    bool secret = false;
    sp::P<sp::gui::Widget> popup_message;
    sp::string message;
    sp::string decode_message;
    sp::Rect2d area;
};

class SecretCube : public sp::Node
{
public:
    SecretCube(sp::P<sp::Node> parent)
    : sp::Node(parent)
    {
        render_data.shader = sp::Shader::get("internal:basic.shader");
        render_data.type = sp::RenderData::Type::Additive;
        render_data.mesh = sp::obj_loader.get("cube.obj");
        render_data.texture = sp::texture_manager.get("cube.png");
        render_data.color = sp::Color(1,1,1, 1.0);
        render_data.scale = sp::Vector3f(0, 0, 0);
        render_data.order = 1000;
        setPosition({0, 0, 5});
        setRotation(sp::Quaterniond::fromAxisAngle({0.6, 0.8, 0.0}, 60) * getRotation3D());
        timer.start(3.0);
    }

    void onUpdate(float delta) override
    {
        setRotation(sp::Quaterniond::fromAxisAngle({sp::random(0, 1), 0.0, 1.0}, delta * 90.0) * getRotation3D());
        if (timer.isExpired()) {
            switch(state) {
            case State::Spawn: state = State::Wait; timer.start(3.0); break;
            case State::Wait: state = State::MoveToTarget; timer.start(10.0); break;
            case State::MoveToTarget: state = State::Done; break;
            case State::Done: break;
            }
        }
        if (state == State::Spawn) render_data.scale = sp::Tween<sp::Vector3f>::easeOutCubic(timer.getProgress(), 0, 1, {0,0,0}, sp::Vector3f(0.5, 0.5, 0.5));
        if (state == State::MoveToTarget) setPosition(sp::Tween<sp::Vector2d>::easeInOutCubic(timer.getProgress(), 0, 1, start, target));
    }

    sp::Vector2d start;
    sp::Vector2d target;
    sp::Timer timer;
    enum class State {
        Spawn,
        Wait,
        MoveToTarget,
        Done,
    } state = State::Spawn;
};

class SecretTrigger : public sp::Node, public SaveProgressInterface
{
public:
    SecretTrigger(sp::P<sp::Node> parent)
    : sp::Node(parent) {
    }

    void onFixedUpdate() override
    {
        if (finished) return;
        if (!player || (player->getPosition2D() - getPosition2D()).length() > 2.0) {
            reset();
            return;
        }

        if (key_jump.getDown()) { if (code[step] == 'J') step++; else reset(); }
        if (key_up.getDown()) { if (code[step] == 'U') step++; else reset(); }
        if (key_down.getDown()) { if (code[step] == 'D') step++; else reset(); }
        if (key_left.getDown()) { if (code[step] == 'L') step++; else reset(); }
        if (key_right.getDown()) { if (code[step] == 'R') step++; else reset(); }
        if (code[step] == 'W') {
            if (!wait_timer.isRunning())
                wait_timer.start(sp::stringutil::convert::toFloat(code.substr(step+1)));
            if (wait_timer.isExpired()) {
                step++;
                while(strchr("0123456789.", code[step])) step++;
            }
        }
        if (step == code.length()) {
            finished = true;
            saveGame();

            sp::audio::Sound::play("sfx/secret.wav");
            auto sc = new SecretCube(getParent());
            sc->start = player->getPosition2D() + sp::Vector2d(0, 1.5);
            sc->target = secret_target[key];
            sc->setPosition(sc->start);
        }
    }

    void reset() {
        step = 0;
        wait_timer.stop();
    }

    void save(nlohmann::json& json) override {
        if (finished) json["secret_" + key] = true;
    }
    void load(nlohmann::json& json) override {
        auto it = json.find("secret_" + key);
        if (it != json.end() && bool(*it)) {
            finished = true;
            auto sc = new SecretCube(getParent());
            sc->start = secret_target[key];
            sc->target = secret_target[key];
            sc->setPosition(sc->start);
        }
    }

    bool finished = false;
    size_t step = 0;
    sp::Timer wait_timer;
    sp::string code;
    sp::string key;
};

class TilemapAnimator : public sp::Node
{
public:
    TilemapAnimator(sp::P<sp::Node> parent)
    : sp::Node(parent) {
        timer.repeat(0.3);
    }

    void onUpdate(float delta) override {
        if(timer.isExpired()) {
            tilemaps[active_index]->render_data.type = sp::RenderData::Type::None;
            active_index = (active_index + 1) % tilemaps.size();
            tilemaps[active_index]->render_data.type = sp::RenderData::Type::Normal;
        }
    }

    int active_index = 0;
    std::vector<sp::P<sp::Tilemap>> tilemaps;
    sp::Timer timer;
};

class NormalExit : public sp::Node
{
public:
    NormalExit(sp::P<sp::Node> parent)
    : sp::Node(parent) {
        sp::collision::Box2D shape{0.1, 0.1};
        shape.type = sp::collision::Shape::Type::Sensor;
        setCollisionShape(shape);
    }

    void onCollision(sp::CollisionInfo& info) override
    {
        if (info.other != player) return;
        showMessage("As you leave,\nyou can only wonder,", [](){
            showMessage("Was there more\nto all of this?", [](){
                player->removeCollisionShape();
                sp::gui::Loader::load("gui/ending.gui", "ENDING");
            });
        });
    }
};

class SecretExit : public sp::Node
{
public:
    SecretExit(sp::P<sp::Node> parent)
    : sp::Node(parent) {
        sp::collision::Box2D shape{0.1, 0.1};
        shape.type = sp::collision::Shape::Type::Sensor;
        setCollisionShape(shape);
    }

    void onCollision(sp::CollisionInfo& info) override
    {
        if (info.other != player) return;
        for(sp::P<SecretTrigger> st : getParent()->getChildren()) {
            if (st && !st->finished) return;
        }
        showMessage("You enter the\nmagical doorway", [](){
            showMessage("No idea what paths\nyou will cross next...", [](){
                player->removeCollisionShape();
                sp::gui::Loader::load("gui/secret.ending.gui", "ENDING");
            });
        });
    }
};

sp::P<sp::Window> window;

void createWorld()
{
    auto scene = new sp::Scene("MAIN");
    camera = new sp::Camera(scene->getRoot());
    camera->setOrtographic({5, 7});
    scene->setDefaultCamera(camera);

    std::unordered_map<int, sp::Tilemap::Collision> tile_collision;
    enum class TileSpecial {
        None,
        SpikeUp,
        SpikeDown,
        SpikeLeft,
        SpikeRight,
        Water,
        Moss,
    };
    std::unordered_map<int, TileSpecial> tile_special;
    std::unordered_map<int, std::vector<int>> tile_animations;
    auto json = nlohmann::json::parse(sp::io::ResourceProvider::get("map.json")->readAll());
    for(auto& tile : json["tilesets"][0]["tiles"]) {
        int tile_id = tile["id"];
        if (tile.find("properties") != tile.end()) {
            for(auto& prop : tile["properties"]) {
                std::string name = prop["name"];
                if (name == "solid" && bool(prop["value"]))
                    tile_collision[tile_id] = sp::Tilemap::Collision::Solid;
                if (name == "water" && bool(prop["value"]))
                    tile_special[tile_id] = TileSpecial::Water;
                if (name == "moss" && bool(prop["value"]))
                    tile_special[tile_id] = TileSpecial::Moss;
                if (name == "spikes") {
                    if (std::string(prop["value"]) == "up") tile_special[tile_id] = TileSpecial::SpikeUp;
                    if (std::string(prop["value"]) == "down") tile_special[tile_id] = TileSpecial::SpikeDown;
                    if (std::string(prop["value"]) == "left") tile_special[tile_id] = TileSpecial::SpikeLeft;
                    if (std::string(prop["value"]) == "right") tile_special[tile_id] = TileSpecial::SpikeRight;
                }
            }
        }
        if (tile.find("animation") != tile.end()) {
            std::vector<int> animation;
            for(auto& anim : tile["animation"])
                animation.push_back(int(anim["tileid"]));
            tile_animations[tile_id] = std::move(animation);
        }
    }
    std::unordered_map<size_t, sp::P<TilemapAnimator>> animation_layers;
    for(auto& layer : json["layers"]) {
        if (layer["type"] == "tilelayer") {
            auto tilemap = new sp::Tilemap(scene->getRoot(), "tileset.png", 1.0, 1.0, 10, 10);
            tilemap->render_data.order = -100;
            bool maintilemap = std::string(layer["name"]) == "MAIN";
            tilemap_by_name[std::string(layer["name"])] = tilemap;
            tilemap->setTilemapSpacingMargin(0.01, 0.0);
            sp::Vector2i tile_min{99999,99999};
            sp::Vector2i tile_max{-99999,-99999};
            for(auto& chunk : layer["chunks"]) {
                int x = chunk["x"];
                int y = chunk["y"];
                int w = chunk["width"];
                int h = chunk["height"];
                for(auto p : sp::Rect2i{{0, 0}, {w, h}}) {
                    int tile_nr = int(chunk["data"][p.x + p.y * w]) - 1;
                    if (tile_nr >= 0) {
                        auto tp = sp::Vector2i{p.x + x, - p.y - y - 1};
                        tile_min.x = std::min(tile_min.x, tp.x);
                        tile_min.y = std::min(tile_min.y, tp.y);
                        tile_max.x = std::max(tile_max.x, tp.x);
                        tile_max.y = std::max(tile_max.y, tp.y);
                        if (tile_animations.find(tile_nr) != tile_animations.end()) {
                            const auto& anim = tile_animations[tile_nr];
                            if (animation_layers.find(anim.size()) == animation_layers.end()) {
                                animation_layers[anim.size()] = new TilemapAnimator(scene->getRoot());
                                for(size_t n=0; n<anim.size(); n++) {
                                    auto new_tilemap = new sp::Tilemap(scene->getRoot(), "tileset.png", 1.0, 1.0, 10, 10);
                                    new_tilemap->render_data.order = -99;
                                    if (n > 0) new_tilemap->render_data.type = sp::RenderData::Type::None;
                                    animation_layers[anim.size()]->tilemaps.push_back(new_tilemap);
                                }
                            }
                            for(size_t n=0; n<anim.size(); n++) {
                                animation_layers[anim.size()]->tilemaps[n]->setTile(tp, anim[n], sp::Tilemap::Collision::Open);
                            }
                        } else {
                            tilemap->setTile(tp, tile_nr, maintilemap ? tile_collision[tile_nr] : sp::Tilemap::Collision::Open);
                        }

                        switch(tile_special[tile_nr]) {
                        case TileSpecial::None: break;
                        case TileSpecial::SpikeDown: new KillZone(scene->getRoot(), sp::Vector2d(tp) + sp::Vector2d(0.5, 0.1), sp::Vector2d(0.8, 0.2)); break;
                        case TileSpecial::SpikeUp: new KillZone(scene->getRoot(), sp::Vector2d(tp) + sp::Vector2d(0.5, 0.9), sp::Vector2d(0.8, 0.2)); break;
                        case TileSpecial::SpikeLeft: new KillZone(scene->getRoot(), sp::Vector2d(tp) + sp::Vector2d(0.1, 0.5), sp::Vector2d(0.2, 0.8)); break;
                        case TileSpecial::SpikeRight: new KillZone(scene->getRoot(), sp::Vector2d(tp) + sp::Vector2d(0.9, 0.5), sp::Vector2d(0.2, 0.8)); break;
                        case TileSpecial::Water: watermap.set(tp, true); break;
                        case TileSpecial::Moss: mossmap.set(tp, true); break;
                        }
                    }
                }
            }
            if (layer.find("properties") != layer.end()) {
                for(auto& prop : layer["properties"]) {
                    std::string name = prop["name"];
                    if (name == "autohide" && bool(prop["value"])) {
                        new HideLayerTrigger(tilemap, {sp::Vector2d(tile_min) - sp::Vector2d(0.5, 0.25), sp::Vector2d(tile_max - tile_min) + sp::Vector2d(2, 1.5)});
                    }
                    if (name == "z") {
                        tilemap->render_data.order = int(prop["value"]);
                    }
                }
            }
        }
        if (layer["type"] == "objectgroup") {
            for(auto& obj : layer["objects"]) {
                double x = obj["x"];
                double y = obj["y"];
                sp::Vector2d pos{x / 13.0, -y / 13.0 + 0.5};
                std::string name = obj["name"];
                if (name == "checkpoint") {
                    auto cp = new Checkpoint(scene->getRoot());
                    cp->setPosition(pos);
                    cp->id = obj["id"];
                } else if (name == "tapemeasure") {
                    new Pickup(scene->getRoot(), pos, Pickup::Type::TapeMeasure, obj["id"]);
                } else if (name == "climbingglove") {
                    new Pickup(scene->getRoot(), pos, Pickup::Type::ClimbingGlove, obj["id"]);
                } else if (name == "teleport") {
                    new Pickup(scene->getRoot(), pos, Pickup::Type::Teleport, obj["id"]);
                } else if (name == "diving") {
                    new Pickup(scene->getRoot(), pos, Pickup::Type::DivingHelmet, obj["id"]);
                } else if (name == "spider") {
                    new Pickup(scene->getRoot(), pos, Pickup::Type::RadioactiveSpider, obj["id"]);
                } else if (name == "fallingblock") {
                    new FallingBlock(scene->getRoot(), pos);
                } else if (name == "start") {
                    start_position = pos;
#ifdef DEBUG
                } else if (name == "quickstart") {
                    intro_state = IntroState::Crashed;
                    player = new Player(scene->getRoot());
                    player->setPosition(pos);
                    player->can_hang = true;
                    player->can_teleport = true;
                    player->can_dive = true;
                    player->can_rope = true;
                    player->death_line->render_data.type = sp::RenderData::Type::Normal;
                    start_position = pos;
#endif
                } else if (name == "plane") {
                    plane_start_position = pos;
                } else if (name == "sign") {
                    auto mst = new MessageSignTrigger(scene->getRoot());
                    mst->setPosition(pos);
                    for(auto& prop : obj["properties"]) {
                        std::string prop_name = prop["name"];
                        if (prop_name == "text")
                            mst->message = prop["value"];
                        if (prop_name == "secret" && bool(prop["value"]))
                            mst->secret = true;
                    }
                } else if (name == "secret") {
                    auto st = new SecretTrigger(scene->getRoot());
                    st->setPosition(pos);
                    for(auto& prop : obj["properties"]) {
                        std::string prop_name = prop["name"];
                        if (prop_name == "code")
                            st->code = prop["value"];
                        if (prop_name == "key")
                            st->key = prop["value"];
                    }
                } else if (name == "secret2") {
                    for(auto& prop : obj["properties"]) {
                        std::string prop_name = prop["name"];
                        if (prop_name == "key")
                            secret_target[prop["value"]] = pos - sp::Vector2d(0, 0.5);
                    }
                } else if (name == "normalexit") {
                    auto se = new NormalExit(scene->getRoot());
                    se->setPosition(pos);
                } else if (name == "secretexit") {
                    auto se = new SecretExit(scene->getRoot());
                    se->setPosition(pos);
                }
            }
        }
    }

    auto savedata = sp::io::loadFileContents(sp::io::preferencePath() + "progress.save");
    auto save_json = nlohmann::json::parse(savedata, nullptr, false, false);
    if (!save_json.is_discarded()) {
        if (!player) {
            player = new Player(scene->getRoot());
            for(auto node : sp::Scene::get("MAIN")->getRoot()->getChildren()) {
                auto spi = dynamic_cast<SaveProgressInterface*>(*node);
                if (spi) spi->load(save_json);
            }
            if (player->checkpoint)
                player->setPosition(player->checkpoint->getPosition2D());
        }
    }

    if (player) {
        auto plane = new Plane(scene->getRoot());
        plane->setRotation((start_position - plane_start_position).angle());
        plane->setPosition(start_position);
        plane->engine_emitter.destroy();
        intro_state = IntroState::Done;
        auto pe = new sp::ParticleEmitter(plane, "plane.engine.particles.b.txt");
        pe->setPosition({-0.8, 0});
        pe->setRotation(-plane->getRotation2D());

        camera->setPosition(player->getPosition2D());
        sp::audio::Music::play("music/A Tale of Wind - MP3.ogg");
    } else {
        auto plane = new Plane(scene->getRoot());
        plane->setPosition(plane_start_position);
        plane->setRotation((start_position - plane_start_position).angle());
        camera->setPosition(plane_start_position);
    }
}

int main(int argc, char** argv)
{
    sp::P<sp::Engine> engine = new sp::Engine();

    //Create resource providers, so we can load things.
    sp::io::ResourceProvider::createDefault();

    //Disable or enable smooth filtering by default, enabling it gives nice smooth looks, but disabling it gives a more pixel art look.
    sp::texture_manager.setDefaultSmoothFiltering(false);

    //Create a window to render on, and our engine.
    window = new sp::Window();
    window->setClearColor(sp::Color(0,0,0));
#if !defined(DEBUG) && !defined(EMSCRIPTEN)
    window->setFullScreen(true);
#endif

    sp::gui::Theme::loadTheme("default", "gui/theme/basic.theme.txt");
    new sp::gui::Scene(sp::Vector2d(320, 240));

    sp::P<sp::SceneGraphicsLayer> scene_layer = new sp::SceneGraphicsLayer(1);
    scene_layer->addRenderPass(new sp::BasicNodeRenderPass());
#ifdef DEBUG
    scene_layer->addRenderPass(new sp::CollisionRenderPass());
#endif
    window->addLayer(scene_layer);

    sp::audio::Music::setVolume(50);
    createWorld();
    engine->run();

    return 0;
}
