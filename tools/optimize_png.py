#!/usr/bin/env python3
"""
Optimize PNG files to reduce file size for ESP32 memory constraints
"""

from PIL import Image
import sys
import os

def optimize_png(input_path, output_path=None, max_size_kb=64):
    """
    Optimize PNG file to reduce size while maintaining quality
    
    Args:
        input_path: Path to input PNG file
        output_path: Path to output PNG file (default: overwrites input)
        max_size_kb: Maximum target size in KB
    """
    try:
        # Open image
        img = Image.open(input_path)
        original_size = os.path.getsize(input_path)
        print(f"Original file: {input_path}")
        print(f"Original size: {original_size} bytes ({original_size / 1024:.1f} KB)")
        
        # Ensure it's RGB mode (not RGBA) to reduce size
        if img.mode == 'RGBA':
            # Create white background for transparency
            background = Image.new('RGB', img.size, (255, 255, 255))
            background.paste(img, mask=img.split()[3])  # Use alpha channel as mask
            img = background
            print("Converted RGBA to RGB (removed alpha channel)")
        elif img.mode != 'RGB':
            img = img.convert('RGB')
            print(f"Converted from {img.mode} to RGB")
        
        # If output path not specified, use input path
        if output_path is None:
            output_path = input_path
        
        # Try optimization strategies: standard compression, then quantization if needed
        best_size = original_size
        best_path = input_path
        temp_files = []
        
        # Strategy 1: Maximum compression
        temp_path1 = output_path.replace('.png', '_temp1.png') if output_path != input_path else input_path.replace('.png', '_temp1.png')
        img.save(temp_path1, 'PNG', optimize=True, compress_level=9)
        size1 = os.path.getsize(temp_path1)
        temp_files.append(temp_path1)
        print(f"Strategy 1 (compression): {size1} bytes ({size1 / 1024:.1f} KB)")
        
        if size1 < best_size:
            best_size = size1
            best_path = temp_path1
            if size1 <= max_size_kb * 1024:
                print(f"✓ Target achieved! File is now {size1 / 1024:.1f} KB")
        
        # Strategy 2: Quantization (if needed and first strategy didn't meet target)
        temp_path2 = None
        if best_size > max_size_kb * 1024:
            temp_path2 = output_path.replace('.png', '_temp2.png') if output_path != input_path else input_path.replace('.png', '_temp2.png')
            img_quantized = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
            img_quantized = img_quantized.convert('RGB')
            img_quantized.save(temp_path2, 'PNG', optimize=True, compress_level=9)
            size2 = os.path.getsize(temp_path2)
            temp_files.append(temp_path2)
            print(f"Strategy 2 (quantization): {size2} bytes ({size2 / 1024:.1f} KB)")
            
            if size2 < best_size:
                if best_path != input_path and best_path != output_path and os.path.exists(best_path):
                    os.remove(best_path)
                best_size = size2
                best_path = temp_path2
        
        # Move best result to output path
        if best_path != output_path:
            if os.path.exists(output_path):
                os.remove(output_path)
            if best_path != input_path:
                os.rename(best_path, output_path)
        
        # Clean up remaining temp files
        for temp_file in temp_files:
            if os.path.exists(temp_file) and temp_file != output_path:
                os.remove(temp_file)
        
        final_size = os.path.getsize(output_path)
        reduction = ((original_size - final_size) / original_size) * 100
        
        print(f"\n✓ Optimization complete!")
        print(f"Final size: {final_size} bytes ({final_size / 1024:.1f} KB)")
        print(f"Reduction: {reduction:.1f}%")
        
        if final_size > max_size_kb * 1024:
            print(f"⚠ Warning: File is still {final_size / 1024:.1f} KB (target: {max_size_kb} KB)")
            print("You may need to reduce image quality or dimensions further.")
        else:
            print(f"✓ File is under {max_size_kb} KB target!")
        
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python optimize_png.py <input.png> [output.png] [max_size_kb]")
        print("Example: python optimize_png.py logo.png logo_optimized.png 64")
        print("Note: If output.png is omitted, the input file will be overwritten")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    max_size = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    
    if not os.path.exists(input_file):
        print(f"Error: File not found: {input_file}")
        sys.exit(1)
    
    optimize_png(input_file, output_file, max_size)

