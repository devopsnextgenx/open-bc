from PIL import Image

def change_alpha(input_path, output_path, alpha_factor):
    """
    alpha_factor: float between 0.0 (fully transparent) and 1.0 (fully opaque)
                  or an absolute integer value (0-255).
    """
    img = Image.open(input_path).convert("RGBA")
    
    # Split image into RGBA channels
    r, g, b, a = img.split()
    
    # Scale existing alpha channel by factor (0.0 to 1.0)
    a = a.point(lambda p: int(p * alpha_factor))
    
    # Recombine channels and save
    transparent_img = Image.merge("RGBA", (r, g, b, a))
    transparent_img.save(output_path)
    print(f"Saved modified image to {output_path}")

# Adjust transparency (e.g., 0.5 for 50% opacity)
change_alpha("kamalinee-mukherjee.png", "kamalinee-mukherjee-alpha.png", alpha_factor=0.5)