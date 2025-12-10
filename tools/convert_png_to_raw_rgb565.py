#!/usr/bin/env python3
"""
Convert PNG image to raw RGB565 binary file for LVGL
This creates a file that can be loaded directly as LV_IMG_CF_TRUE_COLOR
"""

from PIL import Image
import sys
import os

def png_to_raw_rgb565(png_path, output_path):
    """Convert PNG to raw RGB565 binary file"""
    try:
        # Open and convert image
        img = Image.open(png_path)
        
        # Get original size
        width, height = img.size
        print(f"Original image size: {width}x{height} pixels")
        
        # Convert to RGB if needed (remove alpha channel)
        if img.mode == 'RGBA':
            # Create white background and composite
            background = Image.new('RGB', (width, height), (255, 255, 255))
            background.paste(img, mask=img.split()[3])  # Use alpha channel as mask
            img = background
        elif img.mode != 'RGB':
            img = img.convert('RGB')
        
        # Get pixel data
        pixels = img.load()
        
        # Convert to RGB565 and write binary
        # LVGL RGB565 format (16-bit):
        # Byte 0 (little-endian low): Green 3 lower bits, Blue 5 bits
        # Byte 1 (little-endian high): Red 5 bits, Green 3 higher bits
        # Bit layout: RRRRRGGG GGGBBBBB (as 16-bit word)
        with open(output_path, 'wb') as f:
            for y in range(height):
                for x in range(width):
                    r, g, b = pixels[x, y]
                    # Convert RGB888 to RGB565
                    # Extract: R 5 bits, G 6 bits, B 5 bits
                    r5 = (r >> 3) & 0x1F  # 5 bits for red
                    g6 = (g >> 2) & 0x3F  # 6 bits for green
                    b5 = (b >> 3) & 0x1F  # 5 bits for blue
                    
                    # Pack into 16-bit word: RRRRRGGG GGGBBBBB
                    # Bits 15-11: Red (5 bits)
                    # Bits 10-5: Green (6 bits)
                    # Bits 4-0: Blue (5 bits)
                    rgb565 = (r5 << 11) | (g6 << 5) | b5
                    
                    # LV_COLOR_16_SWAP is enabled, so we need to swap bytes
                    # Normal format: RRRRRGGG GGGBBBBB
                    # Swapped format: GGGBBBBB RRRRRGGG (for SPI displays)
                    # Extract bytes
                    byte0 = rgb565 & 0xFF  # Low byte: GGGBBBBB
                    byte1 = (rgb565 >> 8) & 0xFF  # High byte: RRRRRGGG
                    # Write swapped: high byte first, then low byte
                    f.write(bytes([byte1, byte0]))
        
        file_size = os.path.getsize(output_path)
        print(f"Successfully converted {png_path} to {output_path}")
        print(f"Image size: {width}x{height} pixels")
        print(f"File size: {file_size} bytes ({file_size / 1024:.2f} KB)")
        print(f"Expected size: {width * height * 2} bytes")
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python convert_png_to_raw_rgb565.py <input.png> [output.raw]")
        print("Example: python convert_png_to_raw_rgb565.py logo.png logo.raw")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else input_file.replace('.png', '.raw')
    
    if not os.path.exists(input_file):
        print(f"Error: File not found: {input_file}")
        sys.exit(1)
    
    png_to_raw_rgb565(input_file, output_file)

