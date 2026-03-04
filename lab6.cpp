#include <opencv2/opencv.hpp>
#include <iostream>
#include <pthread.h>
#include <semaphore.h>
#include <arm_neon.h>
#include <sys/time.h>

#define MEASURE_PERFORMANCE 1

// PMU bit definitions
#define PMCR_E          1    		// enable performance counting registers
#define PMCR_C          (1 << 2)    // reset cycle counter
#define PMCR_P  		(1 << 1)  // reset all event counters
#define PMCNTENSET_C    (1u << 31)  // cycle counter enable
#define PMCNTENSET_E0   1 			// enable performance counting reg 0

// Event codes for Cortex-A76
#define PMU_EVENT_L1D_CACHE_MISS    0x03
#define PMU_EVENT_L2D_CACHE_MISS    0x17

typedef struct arguments {
	cv::Mat* frame;
	int start_row;
	int out_start_row;
	int out_rows;
	int in_start_row;
	int in_rows;
	int num_cols;
	uint64_t elapsed_cycles;
	uint32_t cache_misses;
} arguments;

using namespace cv;
using namespace std;
static cv::Mat to442_grayscale(cv::Mat * frame, int rows, int cols, int start_row);
static cv::Mat to442_sobel(cv::Mat frame);
cv::Mat global_sobel;

int g_x[3][3] = {
    {-1, 0, 1},
    {-2, 0, 2},
    {-1, 0, 1}
};

int g_y[3][3] = {
    {1, 2, 1},
    {0, 0, 0},
    {-1, -2, -1}
};

#if MEASURE_PERFORMANCE
uint64_t total_elapsed_cycles;
uint64_t total_cache_misses;
struct timespec processing_start;
struct timespec processing_end;

static inline void pmu_init(void) {
    uint64_t pmcr;
    asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr)); // read pmcr register and store into pmcr var
    pmcr |= PMCR_E | PMCR_C | PMCR_P;					
	// set bit 0 (E) — enables the PMU counters globally
	// set bit 2 (C) — resets the cycle counter to zero
    asm volatile("msr pmcr_el0, %0" :: "r"(pmcr)); // write the updated pmcr val to register
    asm volatile("msr pmcntenset_el0, %0" :: "r"(PMCNTENSET_C | PMCNTENSET_E0)); // enable the cycle counter and counter 0
	asm volatile("msr pmevtyper0_el0, %0" :: "r"(PMU_EVENT_L1D_CACHE_MISS)); // count cache misses in register 0
}

static inline uint64_t read_cycles(void) {
    uint64_t val;
    asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cache_misses(void) {
    uint32_t val;
    asm volatile("mrs %0, pmevcntr0_el0" : "=r"(val)); // read counter 0
    return val;
}
#endif

static void * process_chunk(void * args) 
{
	#if MEASURE_PERFORMANCE
		uint64_t start; 
		pmu_init();
		start = read_cycles();
	#endif
	int out_start = ((arguments *)args)->out_start_row;
	int out_rows = ((arguments *)args)->out_rows;
	int in_start = ((arguments *)args)->in_start_row;
	int in_rows = ((arguments *)args)->in_rows;
	int cols = ((arguments *)args)->num_cols;
	cv::Mat* frame = ((arguments *)args)->frame;

	cv::Mat gray = to442_grayscale(frame, in_rows, cols, in_start);
	cv::Mat sobel = to442_sobel(gray);

	int local_y0 = out_start - in_start;
    int write_start = out_start;
    int write_end   = out_start + out_rows;

    for (int y = write_start; y < write_end; y++) {
        if (y <= 0 || y >= frame->rows - 1) continue; // can't compute true sobel at global borders
        uchar* srcRow = sobel.ptr<uchar>(local_y0 + (y - out_start));
        uchar* dstRow = global_sobel.ptr<uchar>(y);
        memcpy(dstRow, srcRow, cols);
    }

	#if MEASURE_PERFORMANCE
		uint64_t end = read_cycles();
		((arguments *)args)->elapsed_cycles = end - start;
		((arguments *)args)->cache_misses = read_cache_misses();
	#endif
	return NULL;
}

static cv::Mat to442_grayscale(cv::Mat * frame, int rows, int cols, int start_row)
{
    cv::Mat gray(rows, cols, CV_8UC1);

    for (int y = start_row; y < (start_row + rows); y++) {
        cv::Vec3b * src = frame->ptr<cv::Vec3b>(y);
        uchar* dst = gray.ptr<uchar>(y - start_row);
	int x;
        for (x = 0; x < cols - 16; x+= 16) {
			//using 54 183 19 instead of 0.2126 0.7152 0.0722
			//roughly 256x the value of the above to >> 8

			//loads 16 bgr pixels at a time
			// 3 vectors each holding 16, 8 bit values
			// vld3q_u8 splits into 3
			uint8x16x3_t bgr = vld3q_u8((uint8_t *)(src + x));


			//need to split into lower and upper section due to arithmetic
			// being done on 8 lanes at a time
			uint8x8_t vec54 = vdup_n_u8(54);
			uint8x8_t vec183 = vdup_n_u8(183);
			uint8x8_t vec19 = vdup_n_u8(19);
			//vmull_n_u8 = 8 unsigned 8 bit values multiplied by given value
			//uint16x8_t = 8 lanes of 16 bit values
			//vget gets upper or lower 8 bytes
			uint16x8_t lowr = vmull_u8(vget_low_u8(bgr.val[2]), vec54);
			//vmlal_n_u8 = multiply values by constant then add to accumulator
			//vmlal is used when accumulator (lowg) is larger than factors
			uint16x8_t lowg = vmlal_u8(lowr,vget_low_u8(bgr.val[1]), vec183);
			uint16x8_t gray_low = vmlal_u8(lowg,vget_low_u8(bgr.val[0]),vec19);
			
			uint16x8_t highr = vmull_u8(vget_high_u8(bgr.val[2]), vec54);  //r 
			uint16x8_t highg = vmlal_u8(highr, vget_high_u8(bgr.val[1]), vec183); //r + g
			uint16x8_t gray_high = vmlal_u8(highg, vget_high_u8(bgr.val[0]), vec19); // r + g + b

			//shift right by 8 bits(LCM of previous decimals) and narrow to 8 bit from 16 bit 
			uint8x8_t out_low = vshrn_n_u16(gray_low, 8);
            uint8x8_t out_high = vshrn_n_u16(gray_high, 8);

			//combine the two sets of 8 bytes into 16 lanes of 8 bit values
			uint8x16_t result = vcombine_u8(out_low, out_high);

			//stores 16 grayscale pixels to memory at once
			vst1q_u8(dst+x, result);
        }
		// deals with excess columns if column amt wasnt divisible by 16
		for (; x < cols; x++) {
			uchar b = src[x][0];
			uchar g = src[x][1];
			uchar r = src[x][2];

			dst[x] = (54*r + 183*g + 19*b) >> 8;  
		}
    }
    return gray;
}

static cv::Mat to442_sobel(cv::Mat frame)
{
	int rows = frame.rows;
    int cols = frame.cols;
    cv::Mat sobel = cv::Mat::zeros(frame.rows, frame.cols, CV_32F);

    for (int y = 1; y < rows - 1; y++) {
        uchar* outRow = sobel.ptr<uchar>(y);

        for (int x = 1; x < cols - 1; x+= 8) {
			//initalize to zero
			int16x8_t sum_x = vdupq_n_s16(0);
            int16x8_t sum_y = vdupq_n_s16(0);  
			for (int a = -1; a <= 1; a++) {
                uchar* rowPtr = frame.ptr<uchar>(y + a);
                for (int b = -1; b <= 1; b++) {
					//load 8 pixels from x + b
					// 8 lanes (pixels) 8 bit each
					uint8x8_t pixels = vld1_u8(rowPtr + x + b);
					//convert to 16 bit signed
					int16x8_t signed_16pixels = vreinterpretq_s16_u16(vmovl_u8(pixels));

					int16_t kx = g_x[a+1][b+1];
					int16_t ky = g_y[a +1][b+ 1];
					//vmlaq can be used here because sum and pixels are both 16 bit long
					sum_x = vmlaq_n_s16(sum_x, signed_16pixels, kx);
					sum_y = vmlaq_n_s16(sum_y, signed_16pixels, ky);
                }   
            }
			int16x8_t mag = vaddq_s16(vabsq_s16(sum_x), vabsq_s16(sum_y));
			//move 16 bits into 8 bits and unsigns them
			uint8x8_t out = vqmovun_s16(mag); 

			//stores 8 pixels
			vst1_u8(outRow + x, out);  
        }
        
	}

    return sobel;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <video_file_path>\n";
        return 1;
    }

    const string videoPath = argv[1];
    cv::VideoCapture cap(videoPath);

    const string windowName = "Processed Video";
    cv::namedWindow(windowName, cv::WINDOW_AUTOSIZE);

    if (!cap.isOpened()) {
        cerr << "Error: could not open video: " << videoPath << "\n";
        return 2;
    }

    cv::Mat frame;
	arguments quads[4];
	pthread_t tids[4];
	int i;

	
	long num_frames = 0;
	#if MEASURE_PERFORMANCE
		clock_gettime(CLOCK_MONOTONIC, &processing_start);
	#endif
    while (true) {
        if (!cap.read(frame) || frame.empty()) {
            // End of video (or read error)
            std::cout << "video over or read error\n";
            break;
        }
		
		
		num_frames++;
		global_sobel = cv::Mat(frame.rows, frame.cols, CV_8UC1);

		int amtperquad = frame.rows / 4;
		#if MEASURE_PERFORMANCE
			uint64_t frame_total_cycles = 0;
			int frame_total_cache_misses = 0;
		#endif
		for (i = 0; i < 4; i++) {

			int out_start = i * amtperquad;
			int out_end;
			if (i == 3) {
				out_end = frame.rows;
			} else {
				out_end = (i + 1) * amtperquad;
			}
			int out_rows  = out_end - out_start;

			int in_start = max(0, out_start - 1);
			int in_end   = min(frame.rows, out_end + 1);
			int in_rows  = in_end - in_start;

			quads[i].frame = &frame;
			quads[i].out_start_row = out_start;
			quads[i].out_rows = out_rows;
			quads[i].in_start_row = in_start;
			quads[i].in_rows = in_rows;
			quads[i].num_cols = frame.cols;
			pthread_create(&tids[i], NULL, process_chunk, &quads[i]);
		}

		for (int i = 0; i < 4; i++) {
        	pthread_join(tids[i], NULL);
			#if MEASURE_PERFORMANCE
				frame_total_cycles += quads[i].elapsed_cycles;
				frame_total_cache_misses += quads[i].cache_misses;
			#endif
    	}	
		
		#if MEASURE_PERFORMANCE
			total_elapsed_cycles += frame_total_cycles / 4; // this gets us an average of how many cycles it took for each core to process a quarter of a frame
			total_cache_misses += frame_total_cache_misses / 4;
		#endif

		cv::imshow(windowName, global_sobel);
    	cv::waitKey(1);

    }

    cap.release();
    // cv::destroyAllWindows();

	#if MEASURE_PERFORMANCE 
		clock_gettime(CLOCK_MONOTONIC, &processing_end);
		double time_elapsed = (processing_end.tv_sec - processing_start.tv_sec) + (processing_end.tv_nsec - processing_start.tv_nsec) / 1e9;
		printf("Frames processed: %ld\n", num_frames);
		printf("Time elapsed: %.2f\n", time_elapsed);
		printf("Average frames per second: %.2f\n", num_frames / time_elapsed);
		printf("Average number of cycles per frame per core: %ld\n", total_elapsed_cycles / num_frames);
		printf("Average number of cache misses per frame per core: %lu\n", total_cache_misses / num_frames);
	#endif
    return 0;
}
