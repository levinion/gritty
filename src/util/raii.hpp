#pragma once

#include <utility>
#include <unistd.h>
#include <pixman.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

namespace gritty::util {

// RAII wrapper for POSIX file descriptors
class UniqueFd {
public:
    constexpr UniqueFd() noexcept : fd_(-1) {}
    explicit constexpr UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() noexcept { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    constexpr UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    constexpr UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] constexpr int get() const noexcept { return fd_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }
    constexpr explicit operator bool() const noexcept { return valid(); }

    constexpr int release() noexcept {
        int old = fd_;
        fd_ = -1;
        return old;
    }

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_;
};

// RAII wrapper for pixman_image_t*
class PixmanImage {
public:
    constexpr PixmanImage() noexcept : image_(nullptr) {}
    explicit constexpr PixmanImage(pixman_image_t* img) noexcept : image_(img) {}
    ~PixmanImage() noexcept { reset(); }

    PixmanImage(const PixmanImage&) = delete;
    PixmanImage& operator=(const PixmanImage&) = delete;

    constexpr PixmanImage(PixmanImage&& other) noexcept : image_(other.release()) {}
    constexpr PixmanImage& operator=(PixmanImage&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] constexpr pixman_image_t* get() const noexcept { return image_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return image_ != nullptr; }
    constexpr explicit operator bool() const noexcept { return valid(); }

    constexpr pixman_image_t* release() noexcept {
        pixman_image_t* old = image_;
        image_ = nullptr;
        return old;
    }

    void reset(pixman_image_t* new_img = nullptr) noexcept {
        if (image_ != nullptr) {
            pixman_image_unref(image_);
        }
        image_ = new_img;
    }

    [[nodiscard]] int width() const noexcept {
        return image_ ? pixman_image_get_width(image_) : 0;
    }

    [[nodiscard]] int height() const noexcept {
        return image_ ? pixman_image_get_height(image_) : 0;
    }

    [[nodiscard]] int stride() const noexcept {
        return image_ ? pixman_image_get_stride(image_) : 0;
    }

    [[nodiscard]] uint32_t* data() const noexcept {
        return image_ ? pixman_image_get_data(image_) : nullptr;
    }

private:
    pixman_image_t* image_;
};

// RAII wrapper for pixman_region32_t
class PixmanRegion {
public:
    PixmanRegion() noexcept {
        pixman_region32_init(&region_);
    }

    PixmanRegion(int x, int y, unsigned int width, unsigned int height) noexcept {
        pixman_region32_init_rect(&region_, x, y, width, height);
    }

    ~PixmanRegion() noexcept {
        pixman_region32_fini(&region_);
    }

    PixmanRegion(const PixmanRegion& other) noexcept {
        pixman_region32_init(&region_);
        pixman_region32_copy(&region_, const_cast<pixman_region32_t*>(&other.region_));
    }

    PixmanRegion& operator=(const PixmanRegion& other) noexcept {
        if (this != &other) {
            pixman_region32_copy(&region_, const_cast<pixman_region32_t*>(&other.region_));
        }
        return *this;
    }

    PixmanRegion(PixmanRegion&& other) noexcept {
        pixman_region32_init(&region_);
        pixman_region32_copy(&region_, &other.region_);
        pixman_region32_clear(&other.region_);
    }

    PixmanRegion& operator=(PixmanRegion&& other) noexcept {
        if (this != &other) {
            pixman_region32_copy(&region_, &other.region_);
            pixman_region32_clear(&other.region_);
        }
        return *this;
    }

    [[nodiscard]] pixman_region32_t* get() noexcept { return &region_; }
    [[nodiscard]] const pixman_region32_t* get() const noexcept { return &region_; }

    void clear() noexcept {
        pixman_region32_clear(&region_);
    }

    [[nodiscard]] bool empty() const noexcept {
        return !pixman_region32_not_empty(const_cast<pixman_region32_t*>(&region_));
    }

    void union_rect(int x, int y, unsigned int width, unsigned int height) noexcept {
        pixman_region32_union_rect(&region_, &region_, x, y, width, height);
    }

    void union_region(const PixmanRegion& other) noexcept {
        pixman_region32_union(&region_, &region_, const_cast<pixman_region32_t*>(&other.region_));
    }

    void intersect(const PixmanRegion& other) noexcept {
        pixman_region32_intersect(&region_, &region_, const_cast<pixman_region32_t*>(&other.region_));
    }

    void subtract(const PixmanRegion& other) noexcept {
        pixman_region32_subtract(&region_, &region_, const_cast<pixman_region32_t*>(&other.region_));
    }

    void translate(int x, int y) noexcept {
        pixman_region32_translate(&region_, x, y);
    }

    [[nodiscard]] pixman_box32_t extents() const noexcept {
        return *pixman_region32_extents(const_cast<pixman_region32_t*>(&region_));
    }

    [[nodiscard]] int num_rects() const noexcept {
        int n = 0;
        pixman_region32_rectangles(const_cast<pixman_region32_t*>(&region_), &n);
        return n;
    }

    [[nodiscard]] const pixman_box32_t* rects(int* n_rects) const noexcept {
        return pixman_region32_rectangles(const_cast<pixman_region32_t*>(&region_), n_rects);
    }

private:
    pixman_region32_t region_;
};

// RAII wrapper for FT_Library
class FtLibrary {
public:
    FtLibrary() noexcept : lib_(nullptr) {
        FT_Init_FreeType(&lib_);
    }

    ~FtLibrary() noexcept {
        if (lib_) {
            FT_Done_FreeType(lib_);
        }
    }

    FtLibrary(const FtLibrary&) = delete;
    FtLibrary& operator=(const FtLibrary&) = delete;

    FtLibrary(FtLibrary&& other) noexcept : lib_(other.release()) {}
    FtLibrary& operator=(FtLibrary&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] FT_Library get() const noexcept { return lib_; }
    [[nodiscard]] bool valid() const noexcept { return lib_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    FT_Library release() noexcept {
        FT_Library old = lib_;
        lib_ = nullptr;
        return old;
    }

    void reset(FT_Library new_lib = nullptr) noexcept {
        if (lib_) {
            FT_Done_FreeType(lib_);
        }
        lib_ = new_lib;
    }

private:
    FT_Library lib_;
};

// RAII wrapper for FT_Face
class FtFace {
public:
    constexpr FtFace() noexcept : face_(nullptr) {}
    explicit constexpr FtFace(FT_Face face) noexcept : face_(face) {}
    ~FtFace() noexcept { reset(); }

    FtFace(const FtFace&) = delete;
    FtFace& operator=(const FtFace&) = delete;

    constexpr FtFace(FtFace&& other) noexcept : face_(other.release()) {}
    constexpr FtFace& operator=(FtFace&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] constexpr FT_Face get() const noexcept { return face_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return face_ != nullptr; }
    constexpr explicit operator bool() const noexcept { return valid(); }

    constexpr FT_Face release() noexcept {
        FT_Face old = face_;
        face_ = nullptr;
        return old;
    }

    void reset(FT_Face new_face = nullptr) noexcept {
        if (face_) {
            FT_Done_Face(face_);
        }
        face_ = new_face;
    }

private:
    FT_Face face_;
};

// RAII wrapper for hb_font_t*
class HbFont {
public:
    constexpr HbFont() noexcept : font_(nullptr) {}
    explicit constexpr HbFont(hb_font_t* font) noexcept : font_(font) {}
    ~HbFont() noexcept { reset(); }

    HbFont(const HbFont&) = delete;
    HbFont& operator=(const HbFont&) = delete;

    constexpr HbFont(HbFont&& other) noexcept : font_(other.release()) {}
    constexpr HbFont& operator=(HbFont&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] constexpr hb_font_t* get() const noexcept { return font_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return font_ != nullptr; }
    constexpr explicit operator bool() const noexcept { return valid(); }

    constexpr hb_font_t* release() noexcept {
        hb_font_t* old = font_;
        font_ = nullptr;
        return old;
    }

    void reset(hb_font_t* new_font = nullptr) noexcept {
        if (font_) {
            hb_font_destroy(font_);
        }
        font_ = new_font;
    }

private:
    hb_font_t* font_;
};

// RAII wrapper for hb_buffer_t*
class HbBuffer {
public:
    HbBuffer() noexcept : buf_(hb_buffer_create()) {}
    ~HbBuffer() noexcept {
        if (buf_) {
            hb_buffer_destroy(buf_);
        }
    }

    HbBuffer(const HbBuffer&) = delete;
    HbBuffer& operator=(const HbBuffer&) = delete;

    HbBuffer(HbBuffer&& other) noexcept : buf_(other.release()) {}
    HbBuffer& operator=(HbBuffer&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] hb_buffer_t* get() const noexcept { return buf_; }
    [[nodiscard]] bool valid() const noexcept { return buf_ != nullptr && hb_buffer_allocation_successful(buf_); }
    explicit operator bool() const noexcept { return valid(); }

    hb_buffer_t* release() noexcept {
        hb_buffer_t* old = buf_;
        buf_ = nullptr;
        return old;
    }

    void reset(hb_buffer_t* new_buf = nullptr) noexcept {
        if (buf_) {
            hb_buffer_destroy(buf_);
        }
        buf_ = new_buf;
    }

    void clear() noexcept {
        if (buf_) {
            hb_buffer_clear_contents(buf_);
        }
    }

private:
    hb_buffer_t* buf_;
};

} // namespace gritty::util
