#!/usr/bin/env python3
"""
Convert PNG image to raw RGB565 binary file for SPIFFS/LittleFS
Optimized for embedded systems
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
        
        # Convert to RGB if needed
        if img.mode != 'RGB':
            img = img.convert('RGB')
        
        # Get pixel data
        pixels = img.load()
        
        # Convert to RGB565 and write binary
        with open(output_path, 'wb') as f:
            for y in range(height):
                for x in range(width):
                    r, g, b = pixels[x, y]
                    # Convert RGB888 to RGB565
                    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                    # Write as little-endian 16-bit
                    f.write(rgb565.to_bytes(2, byteorder='little'))
        
        file_size = os.path.getsize(output_path)
        print(f"Successfully converted {png_path} to {output_path}")
        print(f"Image size: {width}x{height} pixels")
        print(f"File size: {file_size} bytes ({file_size / 1024:.2f} KB)")
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python convert_to_raw.py <input.png> [output.raw]")
        print("Example: python convert_to_raw.py logo.png logo.raw")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "logo.raw"
    
    if not os.path.exists(input_file):
        print(f"Error: File not found: {input_file}")
        sys.exit(1)
    
    png_to_raw_rgb565(input_file, output_file)

