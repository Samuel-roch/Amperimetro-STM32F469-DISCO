/*
 * screen.c
 *
 *  Created on: Feb 16, 2023
 *      Author: morgan
 */

#include "main.h"
#include "stm32469i_discovery_lcd.h"
#include <lvgl.h>
#include <screen_driver.h>
#include <stdio.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define LCD_SCREEN_WIDTH OTM8009A_800X480_WIDTH
#define LCD_SCREEN_HEIGHT OTM8009A_800X480_HEIGHT
#define FRAMEBUFFER_SIZE (uint32_t)LCD_SCREEN_HEIGHT*LCD_SCREEN_WIDTH
#define DMA_XFERS_NEEDED FRAMEBUFFER_SIZE/2 // We need half as many transfers because the buffer is an array of
// 16 bits but the transfers are 32 bits.

/*
 * Handles to peripherals
 */
extern DSI_HandleTypeDef hdsi; // From main.c
extern LTDC_HandleTypeDef hltdc;
extern DMA_HandleTypeDef hdma_memtomem_dma2_stream0;
extern DMA2D_HandleTypeDef hdma2d;
/*
 * Global variables
 */
lv_disp_drv_t lv_display_driver;
__attribute__ ( (section(".framebuffer"))) lv_color_t framebuffer_1[FRAMEBUFFER_SIZE];
__attribute__ ( (section(".framebuffer"))) lv_color_t framebuffer_2[FRAMEBUFFER_SIZE];

/*
 * Private functions prototypes
 */
void stm32_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p);
void dma2d_copy_area(lv_area_t area, uint32_t src_buffer, uint32_t dst_buffer);

/*
 * Public functions definitions
 */

void screen_driver_init(){

	BSP_LCD_Init() ;
	BSP_LCD_LayerDefaultInit(0, (uint32_t)framebuffer_1);

	static lv_disp_draw_buf_t draw_buf;
	lv_disp_draw_buf_init(&draw_buf, framebuffer_1, framebuffer_2, FRAMEBUFFER_SIZE);
	lv_disp_drv_init(&lv_display_driver);
	lv_display_driver.direct_mode = true;
	lv_display_driver.hor_res = OTM8009A_800X480_WIDTH;
	lv_display_driver.ver_res = OTM8009A_800X480_HEIGHT;
	lv_display_driver.flush_cb = stm32_flush_cb;
	lv_display_driver.draw_buf = &draw_buf;

	lv_disp_drv_register(&lv_display_driver);
}


void lv_gpu_stm32_dma2d_copy(lv_color_t * buf, lv_coord_t buf_w, const lv_color_t * map, lv_coord_t map_w,
		lv_coord_t copy_w, lv_coord_t copy_h)
{
	DMA2D->CR = 0;
	/* copy output colour mode, this register controls both input and output colour format */
	DMA2D->FGPFCCR = 2; // LV_DMA2D_RGB565;
	DMA2D->FGMAR = (uint32_t)map;
	DMA2D->FGOR = map_w - copy_w;
	DMA2D->OMAR = (uint32_t)buf;
	DMA2D->OOR = buf_w - copy_w;
	DMA2D->NLR = (copy_w << DMA2D_NLR_PL_Pos) | (copy_h << DMA2D_NLR_NL_Pos);

	/* start transfer */
	DMA2D->CR |= DMA2D_CR_START_Msk;

	while(DMA2D->CR & DMA2D_CR_START_Msk);
}

/*
 * Private functions definitions
 */
void stm32_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
	lv_disp_t *disp = _lv_refr_get_disp_refreshing();

	uint16_t *dma_xfer_src, *dma_xfer_dst;

	if(!lv_disp_flush_is_last(disp_drv))
	{
		lv_disp_flush_ready(disp_drv);
		return;
	}

	// Swap the buffer for the one to display and reload the screen at the next vertical blanking
	HAL_LTDC_SetAddress_NoReload(&hltdc, (uint32_t)color_p, 0);
	HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING); // VSYNC

	// Determine source and destination of transfer
	dma_xfer_src = (uint16_t *)color_p;
	if(color_p == framebuffer_1)
	{
		dma_xfer_dst = (uint16_t *)framebuffer_2;
	}
	else
	{
		dma_xfer_dst = (uint16_t *)framebuffer_1;
	}

	for(size_t i = 0; i < disp->inv_p; i++)
	{
		// If the area was not joined (and thus should not be ignored)
		if(!disp->inv_area_joined[i])
		{
			dma2d_copy_area(disp->inv_areas[i], (uint32_t)dma_xfer_src, (uint32_t)dma_xfer_dst);

//			for (int y = disp->inv_areas[i].y1; y <= disp->inv_areas[i].y2; y++)
//			{
//				for (int x = disp->inv_areas[i].x1; x <= disp->inv_areas[i].x2; x++)
//				{
//					dma_xfer_dst[((y * LCD_SCREEN_WIDTH) + x)] = dma_xfer_src[((y * LCD_SCREEN_WIDTH) + x)];
//				}
//			}
		}
	}

	lv_disp_flush_ready(disp_drv);
}

void dma2d_copy_area(lv_area_t area, uint32_t src_buffer, uint32_t dst_buffer)
{
	size_t start_offset = (LCD_SCREEN_WIDTH*(area.y1) + (area.x1))*2; // address offset (not pixel offset so it is multiplied by 2)
	size_t area_width = 1 + area.x2 - area.x1;
	size_t area_height = 1 +  area.y2 - area.y1;
	size_t in_out_offset = LCD_SCREEN_WIDTH - area_width;

	// Set up DMA2D to transfer parts of picture to part of picture
	hdma2d.Init.Mode = DMA2D_M2M;													// plain memory to memory
	hdma2d.Init.ColorMode = DMA2D_OUTPUT_RGB565;
	hdma2d.Init.OutputOffset = in_out_offset;										// nb pixels in buffer between end of area line and start of next area line
	hdma2d.LayerCfg[DMA2D_FOREGROUND_LAYER].InputColorMode = DMA2D_INPUT_RGB565;
	hdma2d.LayerCfg[DMA2D_FOREGROUND_LAYER].InputOffset = in_out_offset;
	hdma2d.LayerCfg[DMA2D_FOREGROUND_LAYER].AlphaMode = DMA2D_NO_MODIF_ALPHA;
	hdma2d.LayerCfg[DMA2D_FOREGROUND_LAYER].InputAlpha = 0;

	HAL_DMA2D_Init(&hdma2d);
	HAL_DMA2D_ConfigLayer(&hdma2d, DMA2D_FOREGROUND_LAYER);
	HAL_DMA2D_Start(&hdma2d, src_buffer + start_offset, dst_buffer + start_offset, area_width, area_height);	// Start transfer
	HAL_DMA2D_PollForTransfer(&hdma2d, 10000);	// Wait for transfer to be over
}
