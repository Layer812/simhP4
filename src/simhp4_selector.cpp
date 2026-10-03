#include "simhp4_selector.h"

#include <M5Unified.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "simhp4_frontend_input.h"

namespace {

struct MachineEntry {
    simhp4_machine_id_t id;
    const char *name;
    const char *subtitle;
    const char *detail1;
    const char *detail2;
};

struct OsEntry {
    simhp4_machine_id_t machine;
    simhp4_os_id_t id;
    const char *name;
    const char *subtitle;
};

static const MachineEntry kMachines[] = {
    {
        SIMHP4_MACHINE_PDP7,
        "PDP-7",
        "18-bit minicomputer",
        "Bell Labs GRAPHIC-II",
        "Release Final savepoint"
    },
    {
        SIMHP4_MACHINE_MICROVAX2,
        "MicroVAX II",
        "32-bit VAX / VAX_630",
        "RQ / MSCP storage",
        "RetroP4 VAX bring-up"
    }
};

static const OsEntry kOs[] = {
    { SIMHP4_MACHINE_PDP7,      SIMHP4_OS_UNIX_V0, "UNIX V0", "PDP-7 historical UNIX" },
    { SIMHP4_MACHINE_MICROVAX2, SIMHP4_OS_BSD43,   "4.3BSD",  "Berkeley UNIX for VAX" }
};

static uint16_t bg()    { return M5.Display.color565(0, 5, 2); }
static uint16_t panel() { return M5.Display.color565(0, 10, 5); }
static uint16_t outer() { return M5.Display.color565(0, 42, 18); }
static uint16_t mid()   { return M5.Display.color565(0, 126, 56); }
static uint16_t core()  { return M5.Display.color565(112, 255, 164); }

static void neon_text(int x, int y, const char *text, float size)
{
    M5.Display.setTextSize(size);

    M5.Display.setTextColor(outer());
    M5.Display.drawString(text, x - 2, y);
    M5.Display.drawString(text, x + 2, y);
    M5.Display.drawString(text, x, y - 2);
    M5.Display.drawString(text, x, y + 2);

    M5.Display.setTextColor(mid());
    M5.Display.drawString(text, x - 1, y);
    M5.Display.drawString(text, x + 1, y);
    M5.Display.drawString(text, x, y - 1);
    M5.Display.drawString(text, x, y + 1);

    M5.Display.setTextColor(core());
    M5.Display.drawString(text, x, y);
}

static void neon_rect(int x, int y, int w, int h)
{
    M5.Display.drawRect(x - 2, y - 2, w + 4, h + 4, outer());
    M5.Display.drawRect(x - 1, y - 1, w + 2, h + 2, mid());
    M5.Display.drawRect(x, y, w, h, core());
}

static void draw_shell(const char *stage, const char *context)
{
    const int w = M5.Display.width();
    const int h = M5.Display.height();

    M5.Display.fillScreen(bg());
    neon_text(30, 18, "simhP4", 3.0f);
    neon_text(30, 72, stage, 2.0f);

    M5.Display.setTextColor(mid());
    M5.Display.setTextSize(1.35f);
    if (context != nullptr)
        M5.Display.drawString(context, 32, 118);

    neon_rect(24, 164, w / 2 - 42, h - 246);
    neon_rect(w / 2 + 18, 164, w / 2 - 42, h - 246);

    M5.Display.setTextSize(1.25f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString("UP / DOWN : SELECT", 30, h - 56);
    M5.Display.drawString("ENTER : NEXT     ESC / BKSP : BACK", w / 2 - 25, h - 56);
}

static void draw_machine(int selected)
{
    const int w = M5.Display.width();
    const int h = M5.Display.height();
    draw_shell("MACHINE SELECT", "Select a machine, then choose its OS");

    const int lx = 48;
    const int ly = 214;
    const int line = 112;
    const int left_w = w / 2 - 42;

    M5.Display.setTextSize(1.45f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString("MACHINE", lx, 180);

    for (int i = 0; i < (int)(sizeof(kMachines) / sizeof(kMachines[0])); ++i) {
        const int y = ly + i * line;
        if (i == selected) {
            M5.Display.fillRect(36, y - 12, left_w - 24, 86, panel());
        }

        M5.Display.setTextSize(2.35f);
        M5.Display.setTextColor(i == selected ? core() : mid());
        M5.Display.drawString(i == selected ? ">" : " ", lx, y);
        M5.Display.drawString(kMachines[i].name, lx + 42, y);

        M5.Display.setTextSize(1.25f);
        M5.Display.setTextColor(mid());
        M5.Display.drawString(kMachines[i].subtitle, lx + 42, y + 48);
    }

    const MachineEntry &m = kMachines[selected];
    const OsEntry *o = nullptr;
    for (int i = 0; i < (int)(sizeof(kOs) / sizeof(kOs[0])); ++i) {
        if (kOs[i].machine == m.id) {
            o = &kOs[i];
            break;
        }
    }

    const int rx = w / 2 + 48;
    M5.Display.setTextSize(1.45f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString("AVAILABLE OS", rx, 180);

    if (o != nullptr) {
        M5.Display.setTextSize(2.55f);
        M5.Display.setTextColor(core());
        M5.Display.drawString(o->name, rx, 230);

        M5.Display.setTextSize(1.35f);
        M5.Display.setTextColor(mid());
        M5.Display.drawString(o->subtitle, rx, 292);
    }

    M5.Display.setTextSize(1.3f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString(m.detail1, rx, 360);
    M5.Display.drawString(m.detail2, rx, 398);

    M5.Display.setTextSize(1.4f);
    M5.Display.setTextColor(core());
    M5.Display.drawString("ENTER  ->  OS SELECT", rx, h - 118);
}

static int os_indices(simhp4_machine_id_t machine, int *dst, int cap)
{
    int count = 0;
    for (int i = 0; i < (int)(sizeof(kOs) / sizeof(kOs[0])); ++i) {
        if (kOs[i].machine == machine && count < cap)
            dst[count++] = i;
    }
    return count;
}

static void draw_os(simhp4_machine_id_t machine, const int *indices, int count, int selected)
{
    const int w = M5.Display.width();
    const int h = M5.Display.height();
    const MachineEntry &m = kMachines[(int)machine];
    draw_shell("OS SELECT", m.name);

    const int lx = 48;
    const int ly = 220;
    const int left_w = w / 2 - 42;

    M5.Display.setTextSize(1.45f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString("OPERATING SYSTEM", lx, 180);

    for (int row = 0; row < count; ++row) {
        const OsEntry &o = kOs[indices[row]];
        const int y = ly + row * 112;

        if (row == selected)
            M5.Display.fillRect(36, y - 12, left_w - 24, 86, panel());

        M5.Display.setTextSize(2.55f);
        M5.Display.setTextColor(row == selected ? core() : mid());
        M5.Display.drawString(row == selected ? ">" : " ", lx, y);
        M5.Display.drawString(o.name, lx + 42, y);

        M5.Display.setTextSize(1.3f);
        M5.Display.setTextColor(mid());
        M5.Display.drawString(o.subtitle, lx + 42, y + 50);
    }

    const OsEntry &o = kOs[indices[selected]];
    const int rx = w / 2 + 48;

    M5.Display.setTextSize(1.45f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString("BOOT TARGET", rx, 180);

    M5.Display.setTextSize(2.25f);
    M5.Display.setTextColor(core());
    M5.Display.drawString(m.name, rx, 230);

    M5.Display.setTextSize(2.55f);
    M5.Display.drawString(o.name, rx, 292);

    M5.Display.setTextSize(1.3f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString(machine == SIMHP4_MACHINE_PDP7
                          ? "Persistent SD disk"
                          : "SD-backed VAX disk (next phase)",
                          rx, 372);

    M5.Display.setTextSize(1.5f);
    M5.Display.setTextColor(core());
    M5.Display.drawString("ENTER  ->  BOOT", rx, h - 118);
}

static simhp4_frontend_key_t wait_key()
{
    simhp4_frontend_key_t key = SIMHP4_FRONTEND_KEY_NONE;
    for (;;) {
        M5.update();
        if (simhp4_frontend_wait_key(&key, 20))
            return key;
        vTaskDelay(1);
    }
}

} // namespace

extern "C" simhp4_selector_choice_t simhp4_selector_run(void)
{
    simhp4_selector_choice_t choice = {
        SIMHP4_MACHINE_PDP7,
        SIMHP4_OS_UNIX_V0
    };

    simhp4_frontend_set_selector_active(1);

    int machine = 0;
    for (;;) {
        draw_machine(machine);
        const simhp4_frontend_key_t key = wait_key();

        if (key == SIMHP4_FRONTEND_KEY_UP) {
            machine = (machine + (int)(sizeof(kMachines) / sizeof(kMachines[0])) - 1)
                    % (int)(sizeof(kMachines) / sizeof(kMachines[0]));
        } else if (key == SIMHP4_FRONTEND_KEY_DOWN) {
            machine = (machine + 1)
                    % (int)(sizeof(kMachines) / sizeof(kMachines[0]));
        } else if (key == SIMHP4_FRONTEND_KEY_ENTER) {
            choice.machine = kMachines[machine].id;
            break;
        }
    }

    int indices[8] = {};
    const int count = os_indices(choice.machine, indices, 8);
    int selected = 0;

    for (;;) {
        draw_os(choice.machine, indices, count, selected);
        const simhp4_frontend_key_t key = wait_key();

        if (key == SIMHP4_FRONTEND_KEY_ESCAPE ||
            key == SIMHP4_FRONTEND_KEY_BACK) {
            return simhp4_selector_run();
        }
        if (key == SIMHP4_FRONTEND_KEY_UP && count > 0) {
            selected = (selected + count - 1) % count;
        } else if (key == SIMHP4_FRONTEND_KEY_DOWN && count > 0) {
            selected = (selected + 1) % count;
        } else if (key == SIMHP4_FRONTEND_KEY_ENTER && count > 0) {
            choice.os = kOs[indices[selected]].id;
            simhp4_frontend_flush();
            simhp4_frontend_set_selector_active(0);
            return choice;
        }
    }
}

extern "C" void simhp4_selector_show_not_ready(simhp4_selector_choice_t choice)
{
    const char *machine = choice.machine == SIMHP4_MACHINE_MICROVAX2
                        ? "MicroVAX II" : "PDP-7";
    const char *os = choice.os == SIMHP4_OS_BSD43 ? "4.3BSD" : "UNIX V0";

    M5.Display.fillScreen(bg());
    neon_text(30, 18, "simhP4", 3.0f);
    neon_text(30, 82, "BOOT TARGET", 2.0f);
    neon_rect(28, 154, M5.Display.width() - 56, M5.Display.height() - 232);

    M5.Display.setTextSize(2.6f);
    M5.Display.setTextColor(core());
    M5.Display.drawString(machine, 58, 205);
    M5.Display.drawString(os, 58, 276);

    M5.Display.setTextSize(1.45f);
    M5.Display.setTextColor(mid());
    M5.Display.drawString("Selector S0 complete.", 58, 372);
    M5.Display.drawString("MicroVAX II core bring-up is next.", 58, 418);
    M5.Display.drawString("PDP-7 Release Final remains unchanged.", 58, 464);
}
