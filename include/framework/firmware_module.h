#pragma once

#include <stddef.h>
#include <stdint.h>

namespace firmware {

struct FirmwareModule {
    const char* name;
    void (*begin)();
    void (*tick)();
    uint8_t beginOrder;
    uint8_t tickOrder;
};

template <size_t Capacity>
class ModuleRegistry {
public:
    bool add(const FirmwareModule& module) {
        if (count_ >= Capacity || module.name == nullptr) return false;
        modules_[count_++] = &module;
        return true;
    }

    void beginAll() const {
        runPhase(true);
    }

    void tickAll() const {
        runPhase(false);
    }

    size_t size() const { return count_; }

    const FirmwareModule* at(size_t index) const {
        return index < count_ ? modules_[index] : nullptr;
    }

private:
    void runPhase(bool beginPhase) const {
        bool visited[Capacity] = {};
        for (size_t pass = 0; pass < count_; ++pass) {
            size_t selected = count_;
            uint8_t selectedOrder = UINT8_MAX;
            for (size_t index = 0; index < count_; ++index) {
                if (visited[index]) continue;
                const uint8_t order = beginPhase
                    ? modules_[index]->beginOrder
                    : modules_[index]->tickOrder;
                if (selected == count_ || order < selectedOrder) {
                    selected = index;
                    selectedOrder = order;
                }
            }
            if (selected == count_) return;
            visited[selected] = true;
            void (*hook)() = beginPhase
                ? modules_[selected]->begin
                : modules_[selected]->tick;
            if (hook != nullptr) hook();
        }
    }

    const FirmwareModule* modules_[Capacity] = {};
    size_t count_ = 0;
};

} // namespace firmware
