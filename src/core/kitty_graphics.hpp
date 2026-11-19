#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>
#include <optional>
#include "util/raii.hpp"
#include "util/mapped_buffer.hpp"

namespace gritty::core {

enum class KittyAction : uint8_t {
    Transmit = 't',
    TransmitAndDisplay = 'T',
    Put = 'p',
    Delete = 'd',
    Query = 'q',
};

enum class KittyMedium : char {
    Direct = 'd',
    File = 'f',
    TempFile = 't',
    SharedMem = 's',
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
    int src_x{0}, src_y{0}, src_w{0}, src_h{0}; // Pixel-space crop; 0 size means full image.
    int cursor_policy{1};
};

// A virtual placement created with U=1: it has no screen position and only
// defines the cell rectangle that Unicode placeholders resolve against.
struct KittyVirtualPlacement {
    uint32_t image_id{0};
    uint32_t placement_id{0};
    int cols{0};
    int rows{0};
    int z_index{0};
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
    [[nodiscard]] size_t memory_bytes() const noexcept { return current_memory_; }
    [[nodiscard]] const std::vector<KittyVirtualPlacement>& virtual_placements() const noexcept { return virtual_placements_; }
    [[nodiscard]] const KittyVirtualPlacement* find_virtual_placement(
        uint32_t image_id, uint32_t placement_id) const noexcept;

    // Let the terminal own the render-side copy of placements and image
    // deletions (keeps the grid's scroll/erase hooks authoritative).
    using PlacementHandler = std::function<void(const KittyPlacement& placement, pixman_image_t* image)>;
    using DeleteHandler = std::function<void(uint32_t image_id)>;
    using PlacementDeleteHandler = std::function<void(const KittyPlacement&)>;
    using ClearImagesHandler = std::function<void()>;
    void set_placement_handler(PlacementHandler handler) { placement_handler_ = std::move(handler); }
    void set_delete_handler(DeleteHandler handler) { delete_handler_ = std::move(handler); }
    void set_placement_delete_handler(PlacementDeleteHandler handler) { placement_delete_handler_ = std::move(handler); }
    void set_clear_images_handler(ClearImagesHandler handler) { clear_images_handler_ = std::move(handler); }

    void delete_placements_at(int col, int row);
    void delete_placements(const KittyPlacement& placement);
    void delete_placements_by_z(int z_index);
    void clear_placements();
    void delete_image(uint32_t image_id);
    void clear();
    // Idle cleanup: drop retained capacity from chunked base64 transfers and
    // placement tables once no image is left on screen. Never called per frame.
    void release_temporary_buffers();

private:
    std::optional<util::PixmanImage> decode_image(
        std::span<const uint8_t> data,
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
        KittyMedium medium{KittyMedium::Direct};
        KittyFormat format{KittyFormat::Rgba};
        bool compressed{false};
        int width{0};
        int height{0};
        int cols{0};
        int rows{0};
        int z_index{0};
        int unicode_placeholder{0};
        int quiet{0};
        int cursor_policy{1};
        char delete_target{'a'};
        int src_x{0}, src_y{0}, src_w{0}, src_h{0};
        std::string buffer;
        bool in_progress{false};
    } pending_chunk_;

    struct KittyRequest {
        KittyAction action{KittyAction::TransmitAndDisplay};
        KittyMedium medium{KittyMedium::Direct};
        KittyFormat format{KittyFormat::Rgba};
        bool compressed{false};
        uint32_t image_id{0};
        bool has_image_id{false};
        uint32_t placement_id{0};
        int width{0}, height{0}, cols{0}, rows{0};
        int z_index{0}, unicode_placeholder{0}, quiet{0}, cursor_policy{1};
        char delete_target{'a'};
        int src_x{0}, src_y{0}, src_w{0}, src_h{0};
        char feature{0};
        int more_data{0};
    };

    static KittyRequest parse_control(std::string_view control_part,
                                      const PendingChunkTransmission& pending);
    bool handle_delete(const KittyRequest& request, int cursor_col, int cursor_row);
    bool load_source(const KittyRequest& request, std::string_view payload,
                     std::vector<uint8_t>& decoded, util::MappedBuffer& mapping,
                     std::span<const uint8_t>& source) const;
    bool store_image(const KittyRequest& request, std::span<const uint8_t> source);

    PlacementHandler placement_handler_;
    DeleteHandler delete_handler_;
    PlacementDeleteHandler placement_delete_handler_;
    ClearImagesHandler clear_images_handler_;
    std::unordered_map<uint32_t, KittyImage> images_;
    std::vector<KittyVirtualPlacement> virtual_placements_;
    std::vector<KittyPlacement> placements_;
};

} // namespace gritty::core
