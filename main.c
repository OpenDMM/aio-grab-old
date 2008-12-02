/* 
AiO Dreambox Screengrabber v0.8

written 2006 - 2008 by Seddi
Contact: seddi@ihad.tv / http://www.ihad.tv

This standalone binary will grab the video-picture convert it from
yuv to rgb and resize it, if neccesary, to the same size as the framebuffer or 
vice versa. For the DM7025 (Xilleon) and DM800/DM8000 (Broadcom) the video will be
grabbed directly from the decoder memory.
It also grabs the framebuffer picture in 32Bit, 16Bit or in 8Bit mode with the 
correct colortable in 8Bit mode from the main graphics memory, because the 
FBIOGETCMAP is buggy on Vulcan/Pallas boxes and didnt give you the correct color 
map.
Finally it will combine the pixmaps to one final picture by using the framebuffer
alphamap and save it as bmp, jpeg or png file. So you will get the same picture 
as you can see on your TV Screen.

There are a few command line switches, use "grab -h" to get them listed.

A special Thanx to tmbinc and ghost for the needed decoder memory information and 
the great support.

Feel free to use the code for your own projects. See LICENSE file for details.
*/

#define GRAB_VERSION "v0.8"

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/videodev.h>
#include <linux/fb.h>
#include "png.h"
#include "jpeglib.h"

#define CLAMP(x)    ((x < 0) ? 0 : ((x > 255) ? 255 : x))
#define SWAP(x,y) 	x^=(y^=(x^=y))

#define RED565(x)    ((((x) >> (11 )) & 0x1f) << 3)
#define GREEN565(x)  ((((x) >> (5 )) & 0x3f) << 2)
#define BLUE565(x)   ((((x) >> (0)) & 0x1f) << 3)

#define YFB(x)    ((((x) >> (10)) & 0x3f) << 2)
#define CBFB(x)  ((((x) >> (6)) & 0xf) << 4)
#define CRFB(x)   ((((x) >> (2)) & 0xf) << 4)
#define BFFB(x)   ((((x) >> (0)) & 0x3) << 6)

#define VIDEO_DEV "/dev/video"

// dont change SPARE_RAM and DMA_BLOCKSIZE until you really know what you are doing !!!
#define SPARE_RAM 250*1024*1024
#define DMA_BLOCKSIZE 0x3FF000

void getvideo(unsigned char *video, int *xres, int *yres);
void getosd(unsigned char *osd, unsigned char *osd_alpha, int *xres, int *yres);
void smooth_resize(unsigned char *source, unsigned char *dest, int xsource, int ysource, int xdest, int ydest, int colors);
void fast_resize(unsigned char *source, unsigned char *dest, int xsource, int ysource, int xdest, int ydest, int colors);
void (*resize)(unsigned char *source, unsigned char *dest, int xsource, int ysource, int xdest, int ydest, int colors);
void combine(unsigned char *output, unsigned char *video, unsigned char *osd, unsigned char *osd_alpha, int xres, int yres);
char* upcase(char* mixedstr);


enum {UNKNOWN,PALLAS,VULCAN,XILLEON,BRCM7401,BRCM4380};
char *stb_name[]={"unknown","Pallas","Vulcan","Xilleon","Brcm7401","Brcm4380"};
int stb_type=UNKNOWN;

// main program

int main(int argc, char **argv) {

	printf("AiO Dreambox Screengrabber "GRAB_VERSION"\n\n");

	int xres_v,yres_v,xres_o,yres_o,xres,yres,aspect;
	int c,osd_only,video_only,use_osd_res,width,use_png,use_jpg,jpg_quality,no_aspect,use_letterbox;

	// we use fast resize as standard now
	resize = &fast_resize;
	
	osd_only=video_only=use_osd_res=width=use_png=use_jpg=no_aspect=use_letterbox=0;
	jpg_quality=50;
	aspect=1;
	
	unsigned char *video, *osd, *osd_alpha, *output;
	video = (unsigned char *)malloc(1920*1080*3);
	osd = (unsigned char *)malloc(1920*1080*3);	
	osd_alpha = (unsigned char *)malloc(1920*1080);	
	output = (unsigned char *)malloc(1920*1080*3);
	
	char filename[256];
	sprintf(filename,"/tmp/screenshot.bmp");
	
	// detect STB
	char buf[256];
	FILE *pipe=popen("cat /proc/fb","r");
	while (fgets(buf,sizeof(buf),pipe))
	{
		if (strstr(upcase(buf),"VULCAN")) {stb_type=VULCAN;}
		if (strstr(upcase(buf),"PALLAS")) {stb_type=PALLAS;}
		if (strstr(upcase(buf),"XILLEON")) {stb_type=XILLEON;}
		if (strstr(upcase(buf),"BCM7401") || strstr(upcase(buf),"BCMFB")) {stb_type=BRCM7401;}
	}
	pclose(pipe);
	if (stb_type == BRCM7401) // Bcrm7401 + Bcrm4380 use the same framebuffer string, so fall back to /proc/cpuinfO for detecting DM8000
	{
		pipe=popen("cat /proc/cpuinfo","r");
		while (fgets(buf,sizeof(buf),pipe))
		{
			if (strstr(upcase(buf),"BRCM4380")) {stb_type=BRCM4380;}
		}
		pclose(pipe);
	}
	if (stb_type == UNKNOWN)
	{
		printf("Unknown STB .. quit.\n");
		return 1;
	} else
	{
		printf("Detected STB: %s\n",stb_name[stb_type]);
	}
	
	// process command line
	while ((c = getopt (argc, argv, "dhj:lbnopr:v")) != -1)
	{
		switch (c)
		{
			case 'h':
			case '?':
				printf("Usage: grab [commands] [filename]\n\n");
				printf("command:\n");
				printf("-o only grab osd (framebuffer)\n");
				printf("-v only grab video\n");
				printf("-d always use osd resolution (good for skinshots)\n");
				printf("-n dont correct 16:9 aspect ratio\n");
				printf("-r (size) resize to a fixed width, maximum: 1920\n");
				printf("-l always 4:3, create letterbox if 16:9\n");
				printf("-b use bicubic picture resize (slow but smooth)\n");
				printf("-j (quality) produce jpg files instead of bmp (quality 0-100)\n");	
				printf("-p produce png files instead of bmp\n");
				printf("-h this help screen\n\n");

				printf("If no command is given the complete picture will be grabbed.\n");
				printf("If no filename is given /tmp/screenshot.[bmp/jpg/png] will be used.\n");
				return 0;
				break;
			case 'o': // OSD only
				osd_only=1;
				video_only=0;	
				break;
			case 'v': // Video only
				video_only=1;
				osd_only=0;
				break;
			case 'd': // always use OSD resolution
				use_osd_res=1;
				no_aspect=1;
				break;
			case 'r': // use given resolution
				width=atoi(optarg);
				if (width > 1920)
				{
					printf("Error: -r (size) ist limited to 1920 pixel !\n");
					return 1;
				}
				break;
			case 'l': // create letterbox
				use_letterbox=1;
				break;
			case 'b': // use bicubic resizing
				resize = &smooth_resize;
				break;			
			case 'p': // use png file format
				use_png=1;
				use_jpg=0;	
				sprintf(filename,"/tmp/screenshot.png");
				break;
			case 'j': // use jpg file format
				use_jpg=1;
				use_png=0;
				jpg_quality=atoi(optarg);
				sprintf(filename,"/tmp/screenshot.jpg");
				break;
			case 'n':
				no_aspect=1;
				break;
		} 
	}
	if (optind < argc) // filename
		sprintf(filename,"%s",argv[optind]);


	// get osd
	if (!video_only)
		getosd(osd,osd_alpha,&xres_o,&yres_o);
	
	// get video
	if (!osd_only)
		getvideo(video,&xres_v,&yres_v);
	
	// get aspect ratio
	if (stb_type == VULCAN || stb_type == PALLAS)
	{
		pipe=popen("cat /proc/bus/bitstream","r");
		while (fgets(buf,sizeof(buf),pipe))
			sscanf(buf,"A_RATIO: %d",&aspect); 
		pclose(pipe);
	} else
	{
		pipe=popen("cat /proc/stb/vmpeg/0/aspect","r");
		while (fgets(buf,sizeof(buf),pipe))
			sscanf(buf,"%x",&aspect); 
		pclose(pipe);
	}
	
	// resizing
 	if (video_only)
	{
		xres=xres_v;
		yres=yres_v;
	} else if (osd_only)
	{
		xres=xres_o;
		yres=yres_o;
	} else if (xres_o == xres_v && yres_o == yres_v)
	{
		xres=xres_v;
		yres=yres_v;
	} else
	{
		if (xres_v > xres_o && !use_osd_res && (width == 0 || width > xres_o))
		{
			// resize osd to video size
			printf("Resizing OSD to %d x %d ...\n",xres_v,yres_v);	
			resize(osd,output,xres_o,yres_o,xres_v,yres_v,3);
			memcpy(osd,output,xres_v*yres_v*3);
			resize(osd_alpha,output,xres_o,yres_o,xres_v,yres_v,1);
			memcpy(osd_alpha,output,xres_v*yres_v);
			xres=xres_v;
			yres=yres_v;
		} else
		{
			// resize video to osd size
			printf("Resizing Video to %d x %d ...\n",xres_o,yres_o);	
			resize(video,output,xres_v,yres_v,xres_o,yres_o,3);
			memcpy(video,output,xres_o*yres_o*3);
			xres=xres_o;
			yres=yres_o;
		}	
	}
	

	// merge video and osd if neccessary
	if (osd_only)
		memcpy(output,osd,xres*yres*3);
	else if (video_only)
		memcpy(output,video,xres*yres*3);
	else 
	{
		printf("Merge Video with Framebuffer ...\n");
		combine(output,video,osd,osd_alpha,xres,yres);
	}

	
	// resize to specific width ?
	if (width)
	{
		printf("Resizing Screenshot to %d x %d ...\n",width,yres*width/xres);
		resize(output,video,xres,yres,width,(yres*width/xres),3);
		yres=yres*width/xres;
		xres=width;
		memcpy(output,video,xres*yres*3);
	}
	

	// correct aspect ratio
	if (!no_aspect && aspect == 3 && ((float)xres/(float)yres)<1.5)
	{
		printf("Correct aspect ratio to 16:9 ...\n");
		resize(output,video,xres,yres,xres,yres/1.33,3);
		yres/=1.33;
		memcpy(output,video,xres*yres*3);
	}
	
	
	// use letterbox ?
	if (use_letterbox && xres*0.8 != yres && xres*0.8 <= 1080)
	{
		int yres_neu;
		yres_neu=xres*0.8;
		printf("Create letterbox %d x %d ...\n",xres,yres_neu);		
		if (yres_neu > yres)
		{
			int ofs;
			ofs=(yres_neu-yres)>>1;
			memmove(output+ofs*xres*3,output,xres*yres*3);
			memset(output,0,ofs*xres*3);
			memset(output+ofs*xres*3+xres*yres*3,0,ofs*xres*3);
		}
		yres=yres_neu;
	}
	
	
	// saving picture
	printf("Saving %s ...\n",filename);
	FILE *fd2 = fopen(filename, "wr");
	
	
	if (!use_png && !use_jpg)
	{
		// write bmp
		unsigned char hdr[14 + 40];
		int i = 0;
#define PUT32(x) hdr[i++] = ((x)&0xFF); hdr[i++] = (((x)>>8)&0xFF); hdr[i++] = (((x)>>16)&0xFF); hdr[i++] = (((x)>>24)&0xFF);
#define PUT16(x) hdr[i++] = ((x)&0xFF); hdr[i++] = (((x)>>8)&0xFF);
#define PUT8(x) hdr[i++] = ((x)&0xFF);
		PUT8('B'); PUT8('M');
		PUT32((((xres * yres) * 3 + 3) &~ 3) + 14 + 40);
		PUT16(0); PUT16(0); PUT32(14 + 40);
		PUT32(40); PUT32(xres); PUT32(yres);
		PUT16(1);
		PUT16(24);
		PUT32(0); PUT32(0); PUT32(0); PUT32(0); PUT32(0); PUT32(0);
#undef PUT32
#undef PUT16
#undef PUT8
		fwrite(hdr, 1, i, fd2);
		
		int y;
		for (y=yres-1; y>=0 ; y-=1) {
			fwrite(output+(y*xres*3),xres*3,1,fd2);
		}
	} else if (use_png)
	{	
		// write png
		png_bytep *row_pointers;
		png_structp png_ptr;
		png_infop info_ptr;
	  
		png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, (png_voidp)NULL, (png_error_ptr)NULL, (png_error_ptr)NULL);
		info_ptr = png_create_info_struct(png_ptr);
		png_init_io(png_ptr, fd2);

		row_pointers=(png_bytep*)malloc(sizeof(png_bytep)*yres);

		int y;
		for (y=0; y<yres; y++)
			row_pointers[y]=output+(y*xres*3);
		
		png_set_bgr(png_ptr);
		png_set_IHDR(png_ptr, info_ptr, xres, yres, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
		png_write_info(png_ptr, info_ptr);
		png_write_image(png_ptr, row_pointers);
		png_write_end(png_ptr, info_ptr);
		png_destroy_write_struct(&png_ptr, &info_ptr);
		
		free(row_pointers);
	} else 
	{
		// write jpg
		int x,y;
		for (y=0; y<yres; y++)
			for (x=0; x<xres; x++)
				SWAP(output[x*3+y*xres*3],output[x*3+y*xres*3+2]);

		struct jpeg_compress_struct cinfo;
		struct jpeg_error_mgr jerr;
		JSAMPROW row_pointer[1];	
		int row_stride;		
		cinfo.err = jpeg_std_error(&jerr);

		jpeg_create_compress(&cinfo);
		jpeg_stdio_dest(&cinfo, fd2);
		cinfo.image_width = xres; 	
		cinfo.image_height = yres;
		cinfo.input_components = 3;	
		cinfo.in_color_space = JCS_RGB;
		jpeg_set_defaults(&cinfo);
		jpeg_set_quality(&cinfo,jpg_quality, TRUE);
		jpeg_start_compress(&cinfo, TRUE);
		row_stride = xres * 3;
		while (cinfo.next_scanline < cinfo.image_height) 
		{
			row_pointer[0] = & output[cinfo.next_scanline * row_stride];
			(void) jpeg_write_scanlines(&cinfo, row_pointer, 1);
		}
		jpeg_finish_compress(&cinfo);
		jpeg_destroy_compress(&cinfo);
	}
	
	fclose(fd2);	
	
	// Thats all folks 
	printf("... Done !\n");
	
	// clean up
	free(video);
	free(osd);
	free(osd_alpha);
	free(output);

	return 0;
}

// grabing the video picture

void getvideo(unsigned char *video, int *xres, int *yres)
{
	printf("Grabbing Video ...\n");
	
	int mem_fd;
	//unsigned char *memory;
	void *memory;
	if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
		printf("Mainmemory: can't open /dev/mem \n");
		return;
	}

	unsigned char *luma, *chroma, *memory_tmp;
	luma = (unsigned char *)malloc(1); // real malloc will be done later
	chroma = (unsigned char *)malloc(1); // this is just to be sure it get initialized and free() will not segfaulting
	memory_tmp = (unsigned char *)malloc(1);
	int t,stride,res;
	res = stride = 0;
	char buf[256];
	FILE *pipe;

	if (stb_type == BRCM7401 || stb_type == BRCM4380)
	{
		// grab brcm7401/4380 pic from decoder memory
		
		if(!(memory = (unsigned char*)mmap(0, 100, PROT_READ, MAP_SHARED, mem_fd, 0x10100000)))
		{
			printf("Mainmemory: <Memmapping failed>\n");
			return;
		}

		unsigned char data[100];

		int adr,adr2,ofs,ofs2,offset;
		int xtmp,xsub,ytmp,t2,dat1;
		
		res=575;
		
		// wait till we get a useful picture
		while (res == 1079 || res == 575 || res == 479) 
		{
			memcpy(data,memory,100); 
			res=data[0x19]<<8|data[0x18];
		}

		stride=data[0x15]<<8|data[0x14];	
		ofs=(data[0x28]<<8|data[0x27])>>4;
		ofs2=(data[0x2c]<<8|data[0x2b])>>4;
		adr=data[0x1f]<<24|data[0x1e]<<16|data[0x1d]<<8|data[0x1c];
		adr2=data[0x23]<<24|data[0x22]<<16|data[0x21]<<8|data[0x20];
		offset=adr2-adr;
			
		munmap(memory, 100);

		// printf("Stride: %d Res: %d\n",stride,res);
		// printf("Adr: %X Adr2: %X OFS: %d %d\n",adr,adr2,ofs,ofs2);
		
		luma = (unsigned char *)malloc(stride*(ofs));
		chroma = (unsigned char *)malloc(stride*(ofs2+64));	
								
		// grabbing luma & chroma plane from the decoder memory
		if (stb_type == BRCM7401)
		{
			// on dm800 we have direct access to the decoder memory
			if(!(memory_tmp = (unsigned char*)mmap(0, offset + stride*(ofs2+64), PROT_READ, MAP_SHARED, mem_fd, adr)))
			{
				printf("Mainmemory: <Memmapping failed>\n");
				return;
			}
			
			usleep(50000); 	// we try to get a full picture, its not possible to get a sync from the decoder so we use a delay
							// and hope we get a good timing. dont ask me why, but every DM800 i tested so far produced a good
							// result with a 50ms delay
			
		} else if (stb_type == BRCM4380)
		{
			// on dm8000 we have to use dma, so dont change anything here until you really know what you are doing !
			
			memory_tmp = (unsigned char *)malloc(offset+stride*(ofs2+64));
			
			if(!(memory = (unsigned char*)mmap(0, DMA_BLOCKSIZE + 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, SPARE_RAM)))
			{
				printf("Mainmemory: <Memmapping failed>\n");
				return;
			}
			volatile unsigned long *mem_dma;
			if(!(mem_dma = (volatile unsigned long*)mmap(0, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0x10c02000)))
			{
				printf("Mainmemory: <Memmapping failed>\n");
				return;
			}

			int i = 0;
			int tmp_len = DMA_BLOCKSIZE;
			int tmp_size = offset + stride*(ofs2+64);
			for (i=0; i < tmp_size; i += DMA_BLOCKSIZE)
			{
				
				unsigned long *descriptor = memory;

				if (i + DMA_BLOCKSIZE > tmp_size)
					tmp_len = tmp_size - i;
				
				descriptor[0] = /* READ */ adr + i;
				descriptor[1] = /* WRITE */ SPARE_RAM + 0x1000;
				descriptor[2] = 0x40000000 | /* LEN */ tmp_len;
				descriptor[3] = 0;
				descriptor[4] = 0;
				descriptor[5] = 0;
				descriptor[6] = 0;
				descriptor[7] = 0;
				mem_dma[1] = /* FIRST_DESCRIPTOR */ SPARE_RAM;
				mem_dma[3] = /* DMA WAKE CTRL */ 3;
				mem_dma[2] = 1;
				while (mem_dma[5] == 1);
				mem_dma[2] = 0;
		
				memcpy(memory_tmp + i, memory + 0x1000, tmp_len);
			}

			munmap(memory, 0x100000);
			munmap((void *)mem_dma, 0x1000);
			
		}
		
		t=t2=dat1=0;
		xsub=64;

		// decode luma & chroma plane or lets say sort it
		for (xtmp=0; xtmp < stride; xtmp+=64)
		{
			if ((stride-xtmp) < 64) 
				xsub=stride-xtmp;

			dat1=xtmp;
			for (ytmp = 0; ytmp < ofs; ytmp++) 
			{
				memcpy(luma+dat1,memory_tmp+t,xsub); // luma
				t+=64;
				if (ytmp < ofs2)
				{
					memcpy(chroma+dat1,memory_tmp+offset+t2,xsub); // chroma
					t2+=64;			
				}
				dat1+=stride;
			}
		}
		
		if (stb_type == BRCM7401)
			munmap(memory_tmp, offset + stride*(ofs2+64));
		else if (stb_type == BRCM4380)
			free(memory_tmp);

		for (t=0; t< stride*ofs;t+=4)
		{
			SWAP(luma[t],luma[t+3]);
			SWAP(luma[t+1],luma[t+2]);

			if (t< stride*(ofs>>1))
			{
				SWAP(chroma[t],chroma[t+3]);
				SWAP(chroma[t+1],chroma[t+2]);			
			}
		
		}
	} else if (stb_type == XILLEON)
	{
		// grab xilleon pic from decoder memory
		
		if(!(memory = (unsigned char*)mmap(0, 1920*1152*6, PROT_READ, MAP_SHARED, mem_fd, 0x6000000)))
		{
			printf("Mainmemory: <Memmapping failed>\n");
			return;
		}
		
		luma = (unsigned char *)malloc(1920*1080);
		chroma = (unsigned char *)malloc(1920*1080);
		
		int offset=1920*1152*5;	// offset for chroma buffer
		
		pipe=popen("cat /proc/stb/vmpeg/0/xres","r");
		while (fgets(buf,sizeof(buf),pipe))
		{
			sscanf(buf,"%x",&stride); 
		}
		pclose(pipe);
		pipe=popen("cat /proc/stb/vmpeg/0/yres","r");
		while (fgets(buf,sizeof(buf),pipe))
		{
			sscanf(buf,"%x",&res); 
		}
		pclose(pipe);
		
		unsigned char frame_l[1920 * 1080]; // luma frame from video decoder
		unsigned char frame_c[1920 * 540]; // chroma frame from video decoder
		// grab luma buffer from decoder memory	
		memcpy(frame_l,memory,1920*1080); 
		// grab chroma buffer from decoder memory
		memcpy(frame_c,memory+offset,1920*540);
		
		munmap(memory, 1920*1152*6);

		int xtmp,ytmp,t,t2,odd_even,oe2,ysub,xsub;
		int ypart=32;
		int xpart=128;
		t=t2=odd_even=oe2=0;

		// "decode" luma/chroma, there are 128x32pixel blocks inside the decoder mem
		for (ysub=0; ysub<(res/32)+1; ysub++) 
		{
			for (xsub=0; xsub<15; xsub++) // 1920/128=15
			{
				for (ytmp=0; ytmp<ypart; ytmp++)
				{
					for (xtmp=0; xtmp< xpart; xtmp++)
					{
						if (odd_even == 0)
							oe2=0;
						if (odd_even == 1 && xtmp < 64)
							oe2=64;
						if (odd_even == 1 && xtmp >= 64)
							oe2=-64;
						if (xsub*xpart+xtmp+oe2 < stride) 
							memcpy(luma+((xsub*xpart+oe2))+xtmp+(stride*(ytmp+(ysub*ypart))),frame_l+t,1); // luma
						if (ysub < (res/64)+1)
						{
							if (xsub*xpart+xtmp+oe2 < stride) 
								memcpy(chroma+((xsub*xpart+oe2))+xtmp+(stride*(ytmp+(ysub*ypart))),frame_c+t,1); // chroma
							t2++;
						}
						t++;
					}
				}
			}
			odd_even^=1;
		}
	} else if (stb_type == VULCAN || stb_type == PALLAS)
	{
		// grab via v4l device (ppc boxes)
		
		memory_tmp = (unsigned char *)malloc(720 * 576 * 3 + 16);
		
		int fd_video = open(VIDEO_DEV, O_RDONLY);
		if (fd_video < 0)
		{
			printf("could not open /dev/video");
			return;
		}	 
		
		int r = read(fd_video, memory_tmp, 720 * 576 * 3 + 16);
		if (r < 16)
		{
			fprintf(stderr, "read failed\n");
			close(fd_video);
			return;
		}
		close(fd_video);
		
		int *size = (int*)memory_tmp;
		stride = size[0];
		res = size[1];
		
		luma = (unsigned char *)malloc(stride * res);
		chroma = (unsigned char *)malloc(stride * res);
		
		memcpy (luma, memory_tmp + 16, stride * res);
		memcpy (chroma, memory_tmp + 16 + stride * res, stride * res);
		
		free(memory_tmp);
	}

	close(mem_fd);	
	
	
	int Y, U, V, set, set2, y ,x, out1, pos;
	set=t=set2=0;
	Y=U=V=0;
		
	// yuv2rgb conversion (4:2:0)
	printf("... converting Video from YUV to RGB color space\n");
	out1=pos=t=0;
	for (y=0; y < res; y+=1)
	{
		for (x=0; x < stride; x+=1)
		{
			if (set == 0) 
			{
				U=chroma[t++]-128;
				V=chroma[t++]-128;
			}
			set^=1;

			Y=76310*(luma[pos++]-16);
			
			if (stb_type == BRCM7401 || stb_type == BRCM4380 || stb_type == VULCAN || stb_type == PALLAS)
			{
				video[out1++]=CLAMP((Y + 132278*U)>>16);
				video[out1++]=CLAMP((Y - 53294*V - 25690*U)>>16);
				video[out1++]=CLAMP((Y + 104635*V)>>16);
			} else if (stb_type == XILLEON)
			{
				video[out1++]=CLAMP((Y + 104635*V)>>16);
				video[out1++]=CLAMP((Y - 53294*V - 25690*U)>>16);
				video[out1++]=CLAMP((Y + 132278*U)>>16);
			}
		}
		if (set2 == 0)
			t-=stride;
		set2^=1;
	}
	
	// correct yres if neccesary
	if (stb_type == BRCM7401 || stb_type == BRCM4380)
	{
		int yres_tmp;
		pipe=popen("cat /proc/stb/vmpeg/0/yres","r");
		while (fgets(buf,sizeof(buf),pipe))
			sscanf(buf,"%x",&yres_tmp); 
		pclose(pipe);
		memset(video+(res+1)*stride*3,0,(yres_tmp-res)*stride);
		res=yres_tmp;
	}
	
	*xres=stride;
	*yres=res;
	printf("... Video-Size: %d x %d\n",*xres,*yres);
	free(luma);
	free(chroma);
}

// grabing the osd picture

void getosd(unsigned char *osd, unsigned char *osd_alpha, int *xres, int *yres)
{
	int fb,x,y,pos,pos1,pos2,ofs;
	unsigned char *lfb;
	struct fb_fix_screeninfo fix_screeninfo;
	struct fb_var_screeninfo var_screeninfo;
	
	fb=open("/dev/fb/0", O_RDWR);
	if (fb == -1)
	{
		fb=open("/dev/fb0", O_RDWR);
		if (fb == -1)
		{
			printf("Framebuffer failed\n");
			return;
		}
	}
	
	if(ioctl(fb, FBIOGET_FSCREENINFO, &fix_screeninfo) == -1)
	{
		printf("Framebuffer: <FBIOGET_FSCREENINFO failed>\n");
		return;
	}

	if(ioctl(fb, FBIOGET_VSCREENINFO, &var_screeninfo) == -1)
	{
		printf("Framebuffer: <FBIOGET_VSCREENINFO failed>\n");
		return;
	}

	
	if(!(lfb = (unsigned char*)mmap(0, fix_screeninfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0)))
	{
		printf("Framebuffer: <Memmapping failed>\n");
		return;
	}
	
	if ( var_screeninfo.bits_per_pixel == 32 ) 
	{
		printf("Grabbing 32bit Framebuffer ...\n");
	
		// get 32bit framebuffer
		pos=pos1=pos2=0;
		ofs=fix_screeninfo.line_length-(var_screeninfo.xres*4);
		for (y=0; y < var_screeninfo.yres; y+=1)
		{
			for (x=0; x < var_screeninfo.xres; x+=1)
			{
				memcpy(osd+pos1,lfb+pos2,3);// bgr
				pos1+=3;
				pos2+=3;
				osd_alpha[pos++]=lfb[pos2];// tr
				pos2++;
			}
			pos2+=ofs;
		} 
	} else if ( var_screeninfo.bits_per_pixel == 16 )
	{
		printf("Grabbing 16bit Framebuffer ...\n");
		unsigned short color;
		
		// get 16bit framebuffer
		pos=pos1=pos2=0;
		ofs=fix_screeninfo.line_length-(var_screeninfo.xres*2);		
		for (y=0; y < var_screeninfo.yres; y+=1)
		{
			for (x=0; x < var_screeninfo.xres; x+=1)
			{
				color = lfb[pos2] << 8 | lfb[pos2+1];
				pos2+=2;
				
				osd[pos1++] = BLUE565(color); // b
				osd[pos1++] = GREEN565(color); // g
				osd[pos1++] = RED565(color); // r
				osd_alpha[pos++]=0x00; // tr - there is no transparency in 16bit mode
			}
			pos2+=ofs;
		} 
	} else if ( var_screeninfo.bits_per_pixel == 8 )
	{
		printf("Grabbing 8bit Framebuffer ...\n");
		unsigned short color;
	
		// Read Color Palette directly from the main memory, because the FBIOGETCMAP is buggy on dream and didnt
		// gives you the correct colortable !
		int mem_fd;
		unsigned char *memory;
		unsigned short rd[256], gn[256], bl[256], tr[256];
		
		if ((mem_fd = open("/dev/mem", O_RDWR) ) < 0) {
			printf("Mainmemory: can't open /dev/mem \n");
			return;
		}

		if(!(memory = (unsigned char*)mmap(0, fix_screeninfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, fix_screeninfo.smem_start-0x1000)))
		{
			printf("Mainmemory: <Memmapping failed>\n");
			return;
		}
		
		if (stb_type == VULCAN) // DM500/5620 stores the colors as a 16bit word with yuv values, so we have to convert :(
		{
			unsigned short yuv;
			pos2 = 0;
			for (pos1=16; pos1<(256*2)+16; pos1+=2)
			{
				
				yuv = memory[pos1] << 8 | memory[pos1+1];
			
				rd[pos2]=CLAMP((76310*(YFB(yuv)-16) + 104635*(CRFB(yuv)-128))>>16);
				gn[pos2]=CLAMP((76310*(YFB(yuv)-16) - 53294*(CRFB(yuv)-128) - 25690*(CBFB(yuv)-128))>>16);
				bl[pos2]=CLAMP((76310*(YFB(yuv)-16) + 132278*(CBFB(yuv)-128))>>16);
			
				if (yuv == 0) // transparency is a bit tricky, there is a 2 bit blending value BFFB(yuv), but not really used
				{
					rd[pos2]=gn[pos2]=bl[pos2]=0;
					tr[pos2]=0x00;
				} else
					tr[pos2]=0xFF;
				
				pos2++;
			}
		} else if (stb_type == PALLAS) // DM70x0 stores the colors in plain rgb values
		{
			pos2 = 0;
			for (pos1=32; pos1<(256*4)+32; pos1+=4)
			{
				rd[pos2]=memory[pos1+1];
				gn[pos2]=memory[pos1+2];
				bl[pos2]=memory[pos1+3];
				tr[pos2]=memory[pos1];
				pos2++;
			}
		} else
		{
			printf("unsupported framebuffermode\n");
			return;
		}
		close(mem_fd);
		
		// get 8bit framebuffer
		pos=pos1=pos2=0;
		ofs=fix_screeninfo.line_length-(var_screeninfo.xres);		
		for (y=0; y < var_screeninfo.yres; y+=1)
		{
			for (x=0; x < var_screeninfo.xres; x+=1)
			{
				color = lfb[pos2++];
				
				osd[pos1++] = bl[color]; // b
				osd[pos1++] = gn[color]; // g
				osd[pos1++] = rd[color]; // r
				osd_alpha[pos++] = tr[color]; // tr
			}
			pos2+=ofs;
		} 
	}
	close(fb);

	*xres=var_screeninfo.xres;
	*yres=var_screeninfo.yres;
	printf("... Framebuffer-Size: %d x %d\n",*xres,*yres);
}

// bicubic pixmap resizing

void smooth_resize(unsigned char *source, unsigned char *dest, int xsource, int ysource, int xdest, int ydest, int colors)
{
	
	float fx,fy,tmp_f/*,dpixel*/;
	unsigned int xs,ys,xd,yd,dpixel;
	unsigned int c,tmp_i;
	int x,y,t,t1;
	xs=xsource; // x-resolution source
	ys=ysource; // y-resolution source
	xd=xdest; // x-resolution destination
	yd=ydest; // y-resolution destination
	
	// get x scale factor
	fx=(float)(xs-1)/(float)xd;

	// get y scale factor
	fy=(float)(ys-1)/(float)yd;

	unsigned int sx1[xd],sx2[xd],sy1,sy2;
	
	// pre calculating sx1/sx2 for faster resizing
	for (x=0; x<xd; x++) 
	{
		// first x source pixel for calculating destination pixel
		tmp_f=fx*(float)x;
		sx1[x]=(int)tmp_f; //floor()

		// last x source pixel for calculating destination pixel
		tmp_f=(float)sx1[x]+fx;
		sx2[x]=(int)tmp_f;
		if ((float)sx2[x] < tmp_f) {sx2[x]+=1;} //ceil()		
	}
	
	// Scale
	for (y=0; y<yd; y++) 
	{

		// first y source pixel for calculating destination pixel
		tmp_f=fy*(float)y;
		sy1=(int)tmp_f; //floor()

		// last y source pixel for calculating destination pixel
		tmp_f=(float)sy1+fy;
		sy2=(int)tmp_f;
		if ((float)sy2 < tmp_f) {sy2+=1;} //ceil()	

		for (x=0; x<xd; x++) 
		{
			// we do this for every color
			for (c=0; c<colors; c++) 
			{
				// calculationg destination pixel
				tmp_i=0;
				dpixel=0;
		
				for (t1=sy1; t1<sy2; t1++) 
				{
					for (t=sx1[x]; t<=sx2[x]; t++) 
					{
						tmp_i+=(int)source[(t*colors)+c+(t1*xs*colors)];
						dpixel++;
					}
				}
		
				//tmp_f=(float)tmp_i/dpixel;
				//tmp_i=(int)tmp_f;
				//if ((float)tmp_i+0.5 <= tmp_f) {tmp_i+=1;} //round()
				tmp_i=tmp_i/dpixel; // working with integers is not correct, but much faster and +-1 inside the color values doesnt really matter
				
				// writing calculated pixel into destination pixmap
				dest[(x*colors)+c+(y*xd*colors)]=tmp_i;
			}
		}
	}
}

// "nearest neighbor" pixmap resizing

void fast_resize(unsigned char *source, unsigned char *dest, int xsource, int ysource, int xdest, int ydest, int colors)
{
    int x_ratio = (int)((xsource<<16)/xdest) ;
    int y_ratio = (int)((ysource<<16)/ydest) ;

	int x2, y2, c, i ,j;
    for (i=0;i<ydest;i++) {
        for (j=0;j<xdest;j++) {
            x2 = ((j*x_ratio)>>16) ;
            y2 = ((i*y_ratio)>>16) ;
            for (c=0; c<colors; c++)
				dest[((i*xdest)+j)*colors + c] = source[((y2*xsource)+x2)*colors + c] ;
        }                
    }          
}

// combining pixmaps by using an alphamap

void combine(unsigned char *output, unsigned char *video, unsigned char *osd, unsigned char *osd_alpha, int xres, int yres)
{
	int x,y,pos,pos1;
	
	pos=pos1=0;
	for (y=0; y < yres; y+=1)
	{
		for (x=0; x < xres; x+=1)
		{
			output[pos1] =  ( ( video[pos1] * ( 0xFF-osd_alpha[pos] ) ) + ( osd[pos1] * osd_alpha[pos] ) ) >>8;
			pos1++;
			output[pos1] =  ( ( video[pos1] * ( 0xFF-osd_alpha[pos] ) ) + ( osd[pos1] * osd_alpha[pos] ) ) >>8;
			pos1++;
			output[pos1] =  ( ( video[pos1] * ( 0xFF-osd_alpha[pos] ) ) + ( osd[pos1] * osd_alpha[pos] ) ) >>8;
			pos1++;
			pos++;
		}
	}
}

// helpers

char* upcase(char* mixedstr) 
{
	int j;
	for (j=0; j< strlen(mixedstr); ++j)
	{
		mixedstr[j]=toupper(mixedstr[j]);
	} 
	return mixedstr;
}
