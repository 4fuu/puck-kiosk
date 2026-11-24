#include "puck/puck_interaction.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, const char* file, int line,
           const std::string& detail = {}) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: check failed: %s", file, line, expression);
    if (!detail.empty()) {
        std::fprintf(stderr, " (%s)", detail.c_str());
    }
    std::fputc('\n', stderr);
}

void check_near(double actual, double expected, double tolerance, const char* expression,
                const char* file, int line) {
    ++g_checks;
    if (std::fabs(actual - expected) <= tolerance) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "%s:%d: check failed: %s (actual %.9g, expected %.9g)\n", file, line,
                 expression, actual, expected);
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)
#define CHECK_MSG(expression, detail) check((expression), #expression, __FILE__, __LINE__, (detail))
#define CHECK_NEAR(actual, expected, tolerance) \
    check_near((actual), (expected), (tolerance), #actual, __FILE__, __LINE__)

constexpr float kPi = 3.14159265358979323846f;

// A 500 px Puck in a 1280 x 720 stage: image height 500, ground at
// (349.56, 579.24), body center at (349.56, 325.5).
const pk::Rect kPuck{100.0f, 100.0f, 500.0f, 500.0f};
const pk::Rect kStage{0.0f, 0.0f, 1280.0f, 720.0f};

pk::PuckInteraction make_interaction() {
    pk::PuckInteraction interaction;
    interaction.set_bounds(kPuck, kStage);
    return interaction;
}

void step_for(pk::PuckInteraction* interaction, float from, float to, float dt = 1.0f / 60.0f) {
    for (float time = from; time <= to; time += dt) {
        interaction->update(time);
    }
    interaction->update(to);
}

void test_hit_regions() {
    {
        pk::PuckInteraction interaction = make_interaction();
        CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
        CHECK(interaction.pressed());
        interaction.pointer_move(400.0f, 450.0f, 0.1f);
        CHECK(interaction.grabbed());
        CHECK(interaction.grab() == pk::PuckGrab::Body);
    }
    {
        pk::PuckInteraction interaction = make_interaction();
        CHECK(interaction.pointer_down(349.56f, 220.0f, 0.0f));
        interaction.pointer_move(400.0f, 220.0f, 0.1f);
        CHECK(interaction.grabbed());
        CHECK(interaction.grab() == pk::PuckGrab::Turn);
    }
    {
        pk::PuckInteraction interaction = make_interaction();
        CHECK(!interaction.pointer_down(20.0f, 20.0f, 0.0f));
        interaction.pointer_move(400.0f, 400.0f, 0.1f);
        CHECK(!interaction.grabbed());
        CHECK(interaction.grab() == pk::PuckGrab::None);
    }
    {
        // A tap must not start a drag, and neither must a 9 px wobble.
        pk::PuckInteraction interaction = make_interaction();
        CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
        interaction.pointer_move(355.0f, 453.0f, 0.1f);
        CHECK(!interaction.grabbed());
        interaction.pointer_up(355.0f, 453.0f, 0.12f);
        CHECK(!interaction.grabbed());
        CHECK(!interaction.active());
    }
}

void test_tip_and_yaw_limits() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 220.0f, 0.0f));
    interaction.pointer_move(349.56f, 100.0f, 0.1f);
    interaction.pointer_move(349.56f, -2000.0f, 0.2f);
    interaction.update(0.2f);
    CHECK_MSG(std::abs(interaction.turn_tip()) <= 0.45f + 1e-5f,
              std::to_string(interaction.turn_tip()));
    CHECK_MSG(std::abs(interaction.turn_tip()) > 0.44f,
              std::to_string(interaction.turn_tip()));

    interaction.pointer_move(5000.0f, 100.0f, 0.3f);
    interaction.update(0.3f);
    CHECK_MSG(interaction.turn_yaw() > 5.0f, std::to_string(interaction.turn_yaw()));
}

void test_body_roll_clamps() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
    interaction.pointer_move(400.0f, 450.0f, 0.1f);
    float time = 0.1f;
    for (int i = 0; i < 40; ++i) {
        const float x = (i % 2 == 0) ? 5000.0f : -5000.0f;
        time += 0.016f;
        interaction.pointer_move(x, 450.0f, time);
        interaction.update(time);
        CHECK(std::abs(interaction.body_roll()) <= 1.2f + 1e-4f);
    }
}

void test_throw_is_capped() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
    interaction.pointer_move(400.0f, 450.0f, 0.05f);
    interaction.pointer_move(4000.0f, 450.0f, 0.06f);
    interaction.pointer_up(4000.0f, 450.0f, 0.061f);
    CHECK_MSG(interaction.linear_speed() <= 6.0f + 1e-4f,
              std::to_string(interaction.linear_speed()));
    CHECK(interaction.linear_speed() > 1.0f);
}

void test_release_velocity_uses_the_last_window() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 220.0f, 0.0f));
    interaction.pointer_move(400.0f, 220.0f, 0.05f);
    interaction.pointer_move(450.0f, 220.0f, 0.10f);
    interaction.pointer_up(450.0f, 220.0f, 0.11f);
    // (450 - 400) px over 50 ms on a 500 px Puck is 2 heights/s; the turn
    // sensitivity is 0.42 heights per radian.
    CHECK_NEAR(interaction.angular_speed(), 2.0f / 0.42f, 0.3f);
}

void test_body_spring_settles() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
    interaction.pointer_move(400.0f, 430.0f, 0.05f);
    interaction.pointer_move(470.0f, 390.0f, 0.10f);
    interaction.pointer_up(470.0f, 390.0f, 0.11f);
    CHECK(interaction.pose().shift_x > 0.1f);
    CHECK(interaction.active());

    step_for(&interaction, 0.11f, 6.0f);
    CHECK_MSG(!interaction.active(), "body did not settle");
    CHECK_NEAR(interaction.pose().shift_x, 0.0f, 1e-3f);
    CHECK_NEAR(interaction.pose().lift_y, 0.0f, 1e-3f);
    CHECK_NEAR(interaction.pose().roll, 0.0f, 1e-3f);
    CHECK_NEAR(interaction.pose().squash, 0.0f, 1e-2f);
}

void test_turn_spring_settles() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 220.0f, 0.0f));
    interaction.pointer_move(430.0f, 220.0f, 0.05f);
    interaction.pointer_move(500.0f, 200.0f, 0.10f);
    interaction.pointer_up(500.0f, 200.0f, 0.11f);
    CHECK(std::abs(interaction.turn_yaw()) > 0.1f);

    step_for(&interaction, 0.11f, 8.0f);
    CHECK_MSG(!interaction.active(), "turn did not settle");
    CHECK_NEAR(interaction.pose().yaw, 0.0f, 1e-3f);
    CHECK_NEAR(interaction.pose().pitch, 0.0f, 1e-3f);
}

void test_throw_returns_home() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
    interaction.pointer_move(400.0f, 450.0f, 0.05f);
    interaction.pointer_move(470.0f, 380.0f, 0.10f);
    interaction.pointer_move(560.0f, 300.0f, 0.15f);
    interaction.pointer_up(560.0f, 300.0f, 0.16f);
    const float release_x = interaction.pose().shift_x;
    CHECK(release_x > 0.2f);

    float max_x = release_x;
    for (float time = 0.16f; time <= 1.5f; time += 1.0f / 60.0f) {
        interaction.update(time);
        max_x = std::max(max_x, interaction.pose().shift_x);
    }
    CHECK_MSG(max_x >= release_x - 1e-3f, std::to_string(max_x));
    step_for(&interaction, 1.5f, 8.0f);
    CHECK(!interaction.active());
    CHECK_NEAR(interaction.pose().shift_x, 0.0f, 1e-3f);
}

void test_pointer_gaze_window() {
    pk::PuckInteraction interaction = make_interaction();
    interaction.pointer_move(400.0f, 400.0f, 1.0f);
    interaction.update(1.0f);
    CHECK(interaction.pose().pointer_active);
    CHECK_NEAR(interaction.pose().pointer_x, 400.0f, 1e-3f);
    interaction.update(4.1f);
    CHECK(!interaction.pose().pointer_active);
}

void test_reset_releases_state() {
    pk::PuckInteraction interaction = make_interaction();
    CHECK(interaction.pointer_down(349.56f, 450.0f, 0.0f));
    interaction.pointer_move(470.0f, 390.0f, 0.1f);
    interaction.update(0.1f);
    CHECK(interaction.active());
    interaction.reset();
    CHECK(!interaction.active());
    CHECK(!interaction.grabbed());
    CHECK(interaction.grab() == pk::PuckGrab::None);
    CHECK_NEAR(interaction.pose().shift_x, 0.0f, 1e-6f);
    CHECK_NEAR(interaction.pose().yaw, 0.0f, 1e-6f);
    CHECK_NEAR(interaction.pose().roll, 0.0f, 1e-6f);
    CHECK_NEAR(interaction.pose().pointer_x, 0.0f, 1e-6f);
}

void test_tip_limit_is_radians() {
    // The web's `U = 0.45` is the asymptote of `0.45 * tanh(raw / 0.45)`.
    CHECK_NEAR(0.45f * std::tanh(1.0f / 0.45f), 0.4394f, 1e-3f);
    CHECK_NEAR(0.45f * std::tanh(10.0f / 0.45f), 0.45f, 1e-5f);
    CHECK(kPi > 0.0f);
}

struct TestCase {
    const char* name;
    void (*function)();
};

}  // namespace

int main() {
    const TestCase tests[] = {
        {"hit regions", test_hit_regions},
        {"tip and yaw limits", test_tip_and_yaw_limits},
        {"body roll clamps", test_body_roll_clamps},
        {"throw is capped", test_throw_is_capped},
        {"release velocity window", test_release_velocity_uses_the_last_window},
        {"body spring settles", test_body_spring_settles},
        {"turn spring settles", test_turn_spring_settles},
        {"throw returns home", test_throw_returns_home},
        {"pointer gaze window", test_pointer_gaze_window},
        {"reset releases state", test_reset_releases_state},
        {"tip limit is radians", test_tip_limit_is_radians},
    };

    std::printf("[==========] puck-interaction-tests\n");
    for (const TestCase& test : tests) {
        const int failures_before = g_failures;
        std::printf("[ RUN      ] %s\n", test.name);
        test.function();
        if (g_failures == failures_before) {
            std::printf("[       OK ] %s\n", test.name);
        } else {
            std::printf("[   FAILED ] %s\n", test.name);
        }
    }

    if (g_failures == 0) {
        std::printf("[  PASSED  ] %d checks\n", g_checks);
        return 0;
    }
    std::printf("[  FAILED  ] %d of %d checks\n", g_failures, g_checks);
    return 1;
}
