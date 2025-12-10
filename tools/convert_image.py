#!/usr/bin/env python3
"""
Convert PNG image to C array for display
Outputs RGB565 format (16-bit color)
"""

from PIL import Image
import sys
import os

def png_to_bitmap_array(png_path, output_path, array_name="bitmap"):
    """Convert PNG to RGB565 C array"""
    try:
        # Open and convert image
        img = Image.open(png_path)
        
        # Get original size
        width, height = img.size
        print(f"Original image size: {width}x{height} pixels")
        
        # Convert to RGB if needed
        if img.mode != 'RGB':
            img = img.convert('RGB')
        
        # Get pixel data
        pixels = img.load()
        
        # Generate C array
        output = f"// Bitmap image: {os.path.basename(png_path)}\n"
        output += f"// Size: {width}x{height} pixels\n"
        output += f"// Format: RGB565 (16-bit)\n\n"
        output += f"#ifndef {array_name.upper()}_H\n"
        output += f"#define {array_name.upper()}_H\n\n"
        output += f"#include <stdint.h>\n\n"
        output += f"#define {array_name.upper()}_WIDTH  {width}\n"
        output += f"#define {array_name.upper()}_HEIGHT {height}\n\n"
        output += f"const uint16_t {array_name}[] = {{\n"
        
        # Convert pixels to RGB565
        for y in range(height):
            output += "  "
            for x in range(width):
                r, g, b = pixels[x, y]
                # Convert RGB888 to RGB565
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                output += f"0x{rgb565:04X}"
                if x < width - 1 or y < height - 1:
                    output += ","
                if (x + 1) % 8 == 0 and x < width - 1:
                    output += "\n  "
            if y < height - 1:
                output += "\n"
        
        output += "\n};\n\n"
        output += f"#endif // {array_name.upper()}_H\n"
        
        # Write to file
        with open(output_path, 'w') as f:
            f.write(output)
        
        array_size = width * height * 2
        print(f"Successfully converted {png_path} to {output_path}")
        print(f"Image size: {width}x{height} pixels")
        print(f"Array size: {width * height} elements ({array_size} bytes, {array_size / 1024:.2f} KB)")
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python convert_image.py <input.png> [output.h] [array_name]")
        print("Example: python convert_image.py logo.png logo.h cobra_logo")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "bitmap_image.h"
    array_name = sys.argv[3] if len(sys.argv) > 3 else "bitmap"
    
    if not os.path.exists(input_file):
        print(f"Error: File not found: {input_file}")
        sys.exit(1)
    
    png_to_bitmap_array(input_file, output_file, array_name)

