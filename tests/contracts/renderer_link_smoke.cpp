#include <ayther/ayther_renderer.h>

#include <cstdio>
#include <type_traits>

namespace {

template <typename Symbol>
void retain_symbol(Symbol symbol) noexcept {
    [[maybe_unused]] volatile Symbol retained = symbol;
}

void retain_renderer_surface() noexcept {
    using Renderer = ayther::AytherRenderer;

    retain_symbol(&Renderer::init);
    retain_symbol(&Renderer::resize);
    retain_symbol(&Renderer::evict_pack_textures);
    retain_symbol(&Renderer::evict_sprite_texture);
    retain_symbol(&Renderer::prewarm_sprite);
    retain_symbol(&Renderer::prewarm_sprite_mask);
    retain_symbol(&Renderer::poll_disk_sprite_textures);
    retain_symbol(&Renderer::shutdown);
    retain_symbol(&Renderer::is_ready);
    retain_symbol(&Renderer::emu_frame_w);
    retain_symbol(&Renderer::emu_frame_h);
    retain_symbol(&Renderer::copy_target_to_buffer);
    retain_symbol(&Renderer::readback_init);
    retain_symbol(&Renderer::export_frame);
    retain_symbol(&Renderer::readback_shutdown);
    retain_symbol(&Renderer::render);
    retain_symbol(&Renderer::set_checker);
    retain_symbol(&Renderer::checker);
    retain_symbol(&Renderer::set_focus_layer);
    retain_symbol(&Renderer::focus_layer);
    retain_symbol(&Renderer::sub_texture_state);
    retain_symbol(&Renderer::render_image);
    retain_symbol(&Renderer::framebuffer_image);
    retain_symbol(&Renderer::framebuffer_view);
    retain_symbol(&Renderer::framebuffer_sampler);
    retain_symbol(&Renderer::framebuffer_extent);
    retain_symbol(&Renderer::compare_ready);
    retain_symbol(&Renderer::compare_render_image);
    retain_symbol(&Renderer::compare_image);
    retain_symbol(&Renderer::compare_view);
    retain_symbol(&Renderer::compare_sampler);
    retain_symbol(&Renderer::compare_extent);
    retain_symbol(&Renderer::capture_compare);
    retain_symbol(&Renderer::compare_release);
    retain_symbol(&Renderer::readback_compare);
    retain_symbol(&Renderer::capture_compare_now);
}

}  // namespace

int main() {
    using Renderer = ayther::AytherRenderer;
    static_assert(!std::is_copy_constructible_v<Renderer>);
    static_assert(!std::is_copy_assignable_v<Renderer>);
    static_assert(std::is_nothrow_destructible_v<Renderer>);

    retain_renderer_surface();

    const Renderer renderer;
    const bool inert = !renderer.is_ready() &&
                       !renderer.render_image().is_valid() &&
                       !renderer.compare_render_image().is_valid();
    std::printf("  [%s] public AytherRenderer surface links and defaults inert\n",
                inert ? " OK " : "FAIL");
    return inert ? 0 : 1;
}
