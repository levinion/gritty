#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>
#include "util/raii.hpp"

namespace gritty::core {

enum class KittyAction : uint8_t {
    Transmit = 't',
    TransmitAndDisplay = 'T',
    Put = 'p',
    Delete = 'd',
    Query = 'q',
};

enum class KittyFormat : uint8_t {
    Rgb = 24,
    Rgba = 32,
    Png = 100,
};

struct KittyPlacement {
    uint32_t image_id{0};
    uint32_t placement_id{0};
    int col{0};
    int row{0};
    int cols{0};
    int rows{0};
    int z_index{0}; // z < 0 below text, z >= 0 above text
};

struct KittyImage {
    uint32_t id{0};
    util::PixmanImage image;
    size_t byte_size{0};
};

class KittyGraphicsEngine final {
public:
    explicit KittyGraphicsEngine(size_t max_memory_bytes = 128 * 1024 * 1024);
    ~KittyGraphicsEngine() = default;

    KittyGraphicsEngine(const KittyGraphicsEngine&) = delete;
    KittyGraphicsEngine& operator=(const KittyGraphicsEngine&) = delete;

    // Handle APC G sequence payload
    std::string process_apc_sequence(std::string_view payload, int cursor_col, int cursor_row);

    [[nodiscard]] const KittyImage* get_image(uint32_t image_id) const noexcept;
    [[nodiscard]] const std::vector<KittyPlacement>& placements() const noexcept { return placements_; }

    void delete_placements_at(int col, int row);
    void delete_image(uint32_t image_id);
    void clear();

private:
    std::vector<uint8_t> decode_base64(std::string_view input);
    std::optional<util::PixmanImage> decode_image(
        const std::vector<uint8_t>& data,
        KittyFormat format,
        int width,
        int height,
        bool compressed
    );

    size_t max_memory_{128 * 1024 * 1024};
    size_t current_memory_{0};
    uint32_t next_auto_id_{1};

    struct PendingChunkTransmission {
        uint32_t image_id{0};
        uint32_t placement_id{0};
        KittyAction action{KittyAction::TransmitAndDisplay};
        KittyFormat format{KittyFormat::Rgba};
        bool compressed{false};
        int width{0};
        int height{0};
        int cols{0};
        int rows{0};
        int z_index{0};
        std::string buffer;
        bool in_progress{false};
    } pending_chunk_;

    std::unordered_map<uint32_t, KittyImage> images_;
    std::vector<KittyPlacement> placements_;
};

} // namespace gritty::core
