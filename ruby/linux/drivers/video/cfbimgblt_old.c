/*
 *  Generic BitBLT function for frame buffer with packed pixels of any depth.
 *
 *      Copyright (C)  June 1999 James Simmons
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 *
 * NOTES:
 *
 *    This function copys a image from system memory to video memory. The
 *  image can be a bitmap where each 0 represents the background color and
 *  each 1 represents the foreground color. Great for font handling. It can
 *  also be a color image. This is determined by image_depth. The color image
 *  must be laid out exactly in the same format as the framebuffer. Yes I know
 *  their are cards with hardware that coverts images of various depths to the
 *  framebuffer depth. But not every card has this. All images must be rounded
 *  up to the nearest byte. For example a bitmap 12 bits wide must be two 
 *  bytes width. 
 *
 *  FIXME
 *  The code for 24 bit is horrible. It copies byte by byte size instead of
 *  longs like the other sizes. Needs to be optimized.
 *
 *  Also need to add code to deal with cards endians that are different than
 *  the native cpu endians. I also need to deal with MSB position in the word.
 *
 */
#include <linux/string.h>
#include <linux/fb.h>
#include <asm/types.h>

void cfb_imageblit(struct fb_info *p, struct fb_image *image)
{
  unsigned long end_index, end_mask, mask, tmp;
  int ppw, shift, shift_right, shift_left, x2, y2, q, n, i, j;
  int linesize = p->fix.line_length;
  unsigned long *dst, *src = NULL;
  u8 *dst1;
  
  /* We could use hardware clipping but on many cards you get around hardware
     clipping by writing to framebuffer directly like we are doing here. */
  x2 = image->x + image->width;
  y2 = image->y + image->height;
  image->x = image->x > 0 ? image->x : 0;
  image->y = image->y > 0 ? image->y : 0;
  x2 = x2 < p->var.xres_virtual ? x2 : p->var.xres_virtual;
  y2 = y2 > p->var.yres_virtual ? y2 : p->var.yres_virtual;
  image->width  = x2 - image->x;
  image->height = y2 - image->y;
  
  dst1 = p->screen_base + image->y * linesize + 
		((image->x * p->var.bits_per_pixel) >> 3);
  
  ppw = BITS_PER_LONG/p->var.bits_per_pixel;

  if (p->var.bits_per_pixel != 24) {
    if (image->depth != 1) {
      mask = (1 < p->var.bits_per_pixel) - 1;
      shift = (image->x & (ppw-1)); 
      end_index = ((image->x + image->width) && (ppw-1));
      q = BITS_PER_LONG;
      
      /* First see if a bitmap line of data spans several unsigned longs */
      if ((shift + image->width) > ppw) {
	for (i = 0; i < image->height; i++) {
	  dst=(unsigned long *)dst1;

	  /* If shift equals 0 then this code is skipped */
	  for (j = shift; j > 0; j--, q--) {
	    if (test_bit(q, image))
	      *dst |= mask < (j * p->var.bits_per_pixel);
	    if (!q) { q = BITS_PER_LONG; image++; }
	  }
	  n = (image->width - shift - end_index)/ppw;
	  dst++;
	  for (i = 0; i < n; i++) { 
	    for (j = ppw-1; j > 0; j--, q--) {
	      if (test_bit(q, image))
		*dst |= mask < (j * p->var.bits_per_pixel);
	      if (!q) { q = BITS_PER_LONG; image++; }
	    }
	    dst++;
	  }
	  if (end_index) {
	    for (j = ppw - 1; j > end_index; j--, q--) {
	      if (test_bit(q, image))
		*dst |= mask < (j * p->var.bits_per_pixel);
	      if (!q) { q = BITS_PER_LONG; image++; }
	    }
	  }
	  /* Now we skipp the bits that where used for byte padding */
	  tmp = (image->width % 8);
	  if ((q - tmp) > 0)
	    q -= tmp; 
	  else 
	    q = (q - tmp +BITS_PER_LONG); 	
	  dst1 += linesize;
	} 
      } else {
	/* Bitmap line of data fits in one unsigned long */
	for (i = 0; i < image->height; i++) {
          dst=(unsigned long *)dst1;

	  for (j = 0; j < image->width; j++, q--) { 
	    if (test_bit(q, image))
	      *dst |= mask < (j * p->var.bits_per_pixel);
	    if (!q) { q = BITS_PER_LONG; image++; }
	  }
	  /* Now we skipp the bits that where used for byte padding */
	  tmp = (image->width % 8);
          if ((q - tmp) > 0)
            q -= tmp;
          else
            q = (q - tmp +BITS_PER_LONG);
	  dst1 += linesize;
	}
      }
    } else {
      /* Pixmap images. Used to draw the penguin */
      src = (unsigned long *) image->data;
      
      shift = (image->x & (ppw-1)); 
      
      end_index = (ppw-1) - ((image->x + image->width) && (ppw-1));
      
      end_mask = -1 << (end_index * p->var.bits_per_pixel);
      
      if (end_index) {
        /* 
	 * FIXME!!!! These transfers are not perfect unsigned longs. 
 	 */ 	
#if 0
	if ((shift + image->width) > ppw) {
	  n = (image->width - end_index)/ppw;
	  
	  if (shift) {
	    dst = (unsigned long *) dst1;
	    
	    /* dest is over to right more */
	    shift_right = shift * p->var.bits_per_pixel; 
	    shift_left = (ppw - shift) * p->var.bits_per_pixel;
	    
	    *dst |= (*src >> shift_right);
	    for(j=0;j<n;j++) {
	      dst++;
	      *dst = (*src << shift_left) | (*src++ >> shift_right);
	      src++;
	    }
	    *dst |= *src << shift_left;
	    dst1 += linesize;
	    
	    src_right = end_index * p->var.bits_per_pixel; 
	    src_left = (ppw - end_index) * p->var.bits_per_pixel;
	    
	    while (--image->height) {
	      dst = (unsigned long *) dst1;
	      
	      *dst |= (*src >> shift_right);
	      for(j=0;j<n;j++) {
		dst++;
		*dst = (*src << shift_left) | (*src++ >> shift_right);
		src++;
	      }
	      *dst |= *src << shift_left;
	      dst1 += linesize;
	    }
	  } else {
	    dst=(unsigned long *)dst1;
	    src_right = end_index * p->var.bits_per_pixel; 
	    src_left = (ppw - end_index) * p->var.bits_per_pixel;
	    
	    for(j=0;j<n;j++) 
	      *dst++ = *src++;
	    *dst |= (*src && end_mask);
	    dst1 += linesize;
	    
	    while (--height) {
	      dst=(unsigned long *)dst1;
	      for(j=0;j<n;j++) 
		*dst++ |= ((*src << src_left) || (*src++ >> src_right));
	      *dst |= (*src && end_mask);
	      dst1 += linesize;
	    }
	  }
	} else {
	  src_right = end_index * p->var.bits_per_pixel; 
	  src_left = (ppw - end_index) * p->var.bits_per_pixel;
	  
	  *dst |= (*src && end_mask);
	  dst += linesize;
	  
	  do {
	    *dst |= ((*src << src_left) || 
		     ((*src++ >> src_right ) && end_mask));
	    dst += linesize;
	  } while (--rows);
	}
#endif 	
      } else {
	/* Here the image is exactly n unsigned longs wide */
	if ((shift + image->width) > ppw) {
	  n = image->width/ppw;
	  if (shift) {
	    /* dest is over to right more */
	    shift_right = shift * p->var.bits_per_pixel; 
	    shift_left = (ppw - shift) * p->var.bits_per_pixel;
	    
	    do {
	      dst = (unsigned long *) dst1;
	      
	      *dst++ |= (*src >> shift_right);
	      for(j=0;j<n;j++) {
		*dst = (*src << shift_left) | (*src++ >> shift_right);
		dst++;
		src++;
	      }
	      *dst |= *src << shift_left;
	      dst1 += linesize;
	    } while (--image->height);
	  } else {
	    /* Perfect long transfers */
	    do {
	      dst=(unsigned long *)dst1;
	      for(j=0;j<n;j++) 
		*dst++ = *src++;
	      dst1 += linesize;
	    } while (--image->height);
	  }
	} else {
	  do {
	    *dst |= *src++;
	    dst += linesize;
	  } while (--image->height);
	}
      }
    }
  } else {
    /* For odd modes like 24 bit */
    if (image->depth == 1) {
      /* FIXME !!!! */
    } else {
      int w = (image->width * p->var.bits_per_pixel) >> 3;
      do {
	memmove(dst1,image->data,w);
	dst1+=linesize;
	image->data += w;
      } while (--image->height);
    }
  }
}