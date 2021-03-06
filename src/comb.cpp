#include <cstdlib>
#include "comb/comb.h"
// #include "comb/comb_psesudo.h"
#include <std_msgs/Float32.h>
#include "comb/utils.h"

#include <glog/logging.h>
/*
The intensity of the filter function was computed in log intensity state.
There is a particular function to convert that back to the image file.
*/


enum {GAUSSIAN, BILATERAL};

namespace comb{

    Comb_filter::Comb_filter(ros::NodeHandle &nh, ros::NodeHandle nh_private){

        constexpr int INTENSITY_ESTIMATE_PUB_QUEUE_SIZE = 1;
        constexpr double EVENT_RETENTION_DURATION = 30; // seconds. Used for calibrating contrast thresholds.

        std::string working_dir;
        std::string save_dir;

        nh_private.getParam("publish_framerate", publish_framerate_);
        nh_private.getParam("save_dir", save_dir);
        nh_private.getParam("working_dir", working_dir);

        VLOG(1) << "Construction framerate: " << publish_framerate_;

        if (save_dir.empty()){

            save_images_ = false;
        }
        else{

            save_images_ = true;
            save_dir_ = comb::utils::fullpath(working_dir, save_dir);
            if (save_dir_.back() != '/'){

                save_dir_.append("/");
            }

            const int dir_err = system((std::string("mkdir -p ") + save_dir_).c_str());
            if (-1 == dir_err){
                LOG(ERROR) << "Error crearing save directory!";
                return;
            }

            VLOG(1) << "Save image to: " << save_dir_;
        }

        // setup publishers
        image_transport::ImageTransport it_(nh_);
        intensity_estimate_pub_ = it_.advertise("comb/intensity_estimate", INTENSITY_ESTIMATE_PUB_QUEUE_SIZE);

        // flags and counters
        initialised_ = false;

        // low-pass parameter to reach 95% of a constant signal in EVENT_RETENTION_DURATION seconds.
        event_count_cutoff_frequency_ = -std::log(1 - 0.95) / EVENT_RETENTION_DURATION; // rad/s.

        contrast_threshold_on_adaptive_ = 0.1; // fixed by convention
        contrast_threshold_off_adaptive_ = -0.1;

        t_next_publish_ = 0.0;
        t_next_recalibrate_contrast_thresholds_ = 0.0;
        t_next_log_intensity_update_ = 0.0;
        //  event_count_total_ = 0.0;

        // dynamic reconfigure
        dynamic_reconfigure_callback_ = boost::bind(&Comb_filter::reconfigureCallback, this, _1, _2);
        server_.reset(new dynamic_reconfigure::Server<comb::combConfig>(nh_private));
        server_->setCallback(dynamic_reconfigure_callback_);
    }

    Comb_filter::~Comb_filter(){

        intensity_estimate_pub_.shutdown();

        // free the buffer space before exit
        delete[] ring_buffer1_;
        delete[] ring_buffer2_;
    }

    void Comb_filter::eventsCallback(const dvs_msgs::EventArray::ConstPtr &msg){
        // initialise image states
        // VLOG(1) << "Inside callback." ;
        if (!initialised_){

            initialise_image_states(msg->height, msg->width);
        }

        if (msg->events.size() > 0){
            for (int i = 0; i < msg->events.size(); i++){

                const int x = msg->events[i].x;
                const int y = msg->events[i].y;

                if (x > 0 && x < msg->width && y > 0 && y < msg->height){

                    const double ts = msg->events[i].ts.toSec();
                    const bool polarity = msg->events[i].polarity;

                    if (adaptive_contrast_threshold_){

                        update_leaky_event_count(ts, x, y, polarity);
                    }

                    // integral tracking
                    integral_tracking(x, y, polarity);
                    // VLOG(1) << "after integral tracking.";

                    while(ts > t_next_store_){
                        if(t_next_store_ == 0){
                            t_next_store_ = ts;
                        } 

                        grab_delay(x_d1_, int(d1_/mtr_), 1);
                        grab_delay(x_d2_, int(d2_/mtr_), 1);
                        grab_delay(x_d12_, int(d12_/mtr_), 1);

                        grab_delay(y_d1_, int(d1_/mtr_), 2);
                        grab_delay(y_d2_, int(d2_/mtr_), 2);
                        grab_delay(y_d12_, int(d12_/mtr_), 2);
                        // calculate new y0_
                        y0_ = x0_ - x_d1_ - rho2_ * x_d2_ + rho2_ * x_d12_ + rho1_ * y_d1_ + y_d2_ - rho1_ * y_d12_;

                        store2buffer(x0_, y0_);

                        t_next_store_ += mtr_;
                    }

                    if (publish_framerate_ > 0 && ts >= t_next_publish_){
                        
                        // VLOG(1) << "New publish";
                        publish_intensity_estimate(msg->events[i].ts);
                        t_next_publish_ = ts + 1 / publish_framerate_;
                    }
                }
            }
            const double ts = msg->events.back().ts.toSec();

            if(adaptive_contrast_threshold_ && (ts > t_next_recalibrate_contrast_thresholds_)){
                constexpr double contrast_threshold_recalibration_frequency = 20.0; // Hz
                recalibrate_contrast_thresholds(ts);
                t_next_recalibrate_contrast_thresholds_ = ts + 1/contrast_threshold_recalibration_frequency;
            }
        }
    }

    // passing by const reference
    void Comb_filter::initialise_image_states(const uint32_t &rows, const uint32_t &columns){

        log_intensity_state_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        leaky_event_count_on_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        leaky_event_count_off_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        ts_array_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        ts_array_on_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        ts_array_off_ = cv::Mat::zeros(rows, columns, CV_64FC1);

        t_next_publish_ = 0.0;
        t_next_recalibrate_contrast_thresholds_ = 0.0;
        t_next_log_intensity_update_ = 0.0;

        // delayed version of integrated events
        x0_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        x_d1_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        x_d2_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        x_d12_ = cv::Mat::zeros(rows, columns, CV_64FC1);

        // delayed version of output signal
        y0_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        y_d1_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        y_d2_ = cv::Mat::zeros(rows, columns, CV_64FC1);
        y_d12_ = cv::Mat::zeros(rows, columns, CV_64FC1);

        // time delay
        d1_ = 0.01;
        d2_ = 0.001;
        d12_ = d1_ + d2_;

        // delay gain
        rho1_ = 0.99;  // distortion reduce factor
        rho2_ = 0.999; // compensate factor

        initialise_buffer(rows, columns);

        initialised_ = true;

        VLOG(2) << "Initialised!";
    }

// insider whether to put buffer initialisation to the start of the program
    void Comb_filter::initialise_buffer(const uint32_t &rows, const uint32_t &columns){

        // minimum time resolution
        mtr_ = 1e-5;
        t_next_store_ = 0.0;

        buffer_length_ = d12_ / mtr_ + 1;
        buffer_index_ = 0;

        ring_buffer1_ = new cv::Mat[buffer_length_];
        ring_buffer2_ = new cv::Mat[buffer_length_];

        // zero initialisation for all buffers
        for (int i = 0; i < buffer_length_; i++){

            cv::Mat my_zero = cv::Mat::zeros(rows, columns, CV_64FC1);
            my_zero.copyTo(ring_buffer1_[i]);
            my_zero.copyTo(ring_buffer2_[i]);
        }
    }

    // tracking the integral to the current state
    void Comb_filter::integral_tracking(const int x, const int y, const bool polarity){

        double c_times_p;
        if (adaptive_contrast_threshold_){

            c_times_p = (polarity) ? contrast_threshold_on_adaptive_ : contrast_threshold_off_adaptive_;
        }
        else{

            c_times_p = (polarity) ? contrast_threshold_on_user_defined_ : contrast_threshold_off_user_defined_;
        }
        x0_.at<double>(y, x) = x0_.at<double>(y, x) + c_times_p;
    }

    void Comb_filter::store2buffer(const cv::Mat &figx, const cv::Mat &figy){

        // double LOG_INTENSITY_OFFSET = std::log(1.5); // chosen because standard APS frames range from [1, 2].
        // cv::Mat image;

        // fig.copyTo(image);
        // image += LOG_INTENSITY_OFFSET;
        // cv::exp(image, image);         // ~[1, 2]
        // image -= 1;                    // [0, 1]

        // buffer index of input and output is the same

        figx.copyTo(ring_buffer1_[buffer_index_]);
        figy.copyTo(ring_buffer2_[buffer_index_]);

        buffer_index_ ++;

        // ring buffer
        if (buffer_index_ >= buffer_length_){

            buffer_index_ = 0;
        }

        // after the storing, the index is one element ahead
    }

    void Comb_filter::update_leaky_event_count(const double& ts, const int& x, const int& y, const bool& polarity){
        if (polarity){
            // positive ON event
            const double delta_t = (ts - ts_array_on_.at<double>(y, x));

            if (delta_t >= 0){

                leaky_event_count_on_.at<double>(y, x) = std::exp(-event_count_cutoff_frequency_ * delta_t)
                * leaky_event_count_on_.at<double>(y, x) + 1;
                ts_array_on_.at<double>(y, x) = ts;
            }
        }
        else{
        // negative OFF event
            const double delta_t = (ts - ts_array_off_.at<double>(y, x));
            if (delta_t >= 0){

                leaky_event_count_off_.at<double>(y, x) = std::exp(-event_count_cutoff_frequency_ * delta_t)
                * leaky_event_count_off_.at<double>(y, x) + 1;
                ts_array_off_.at<double>(y, x) = ts;
            }
        }
    }

    void Comb_filter::recalibrate_contrast_thresholds(const double& ts){
  
        constexpr double EVENT_DENSITY_MIN = 5e6;
        //first do global update
        cv::Mat decay_factor_on;
        cv::Mat decay_factor_off;
        cv::exp(-event_count_cutoff_frequency_ * (ts - ts_array_on_), decay_factor_on);
        cv::exp(-event_count_cutoff_frequency_ * (ts - ts_array_off_), decay_factor_off);

        leaky_event_count_on_ = leaky_event_count_on_.mul(decay_factor_on);
        leaky_event_count_off_ = leaky_event_count_off_.mul(decay_factor_off);

        ts_array_on_.setTo(ts);
        ts_array_off_.setTo(ts);

        const double sum_on = cv::sum(leaky_event_count_on_)[0];
        const double sum_off = cv::sum(leaky_event_count_off_)[0];
        
        if (sum_on + sum_off > EVENT_DENSITY_MIN){
        
            contrast_threshold_off_adaptive_ = -sum_on / (sum_off + 1e-10) * contrast_threshold_on_adaptive_; // re-calibrate contrast thresholds
        }
    }

    void Comb_filter::grab_delay(cv::Mat& sel, const int i1, const int which_buffer){

        int index = buffer_index_ - i1;
        // cv::Mat sel;

        if(index < 0){
            index = buffer_length_ + index;
        }

        if(which_buffer == 1){
            sel = ring_buffer1_[index];
        }
        else{
            sel = ring_buffer2_[index];
        }

        // return sel;
    }

    void Comb_filter::publish_intensity_estimate(const ros::Time &timestamp){

        cv::Mat display_image;
        cv_bridge::CvImage cv_image;

        convert_log_intensity_state_to_display_image(display_image, timestamp.toSec());

        if (color_image_){

            cv::Mat color_display_image;
            cv::cvtColor(display_image, color_display_image, CV_BayerBG2BGR_EA);
            display_image = color_display_image;
            cv_image.encoding = "bgr8";
        }
        else{

            cv_image.encoding = "mono8";
        }

        if (spatial_filter_sigma_ > 0){

            cv::Mat filtered_display_image;
            if (spatial_smoothing_method_ == GAUSSIAN){

                cv::GaussianBlur(display_image, filtered_display_image, cv::Size(5, 5), spatial_filter_sigma_, spatial_filter_sigma_);
            }
            else if (spatial_smoothing_method_ == BILATERAL){

                const double bilateral_sigma = spatial_filter_sigma_ * 25;
                cv::bilateralFilter(display_image, filtered_display_image, 5, bilateral_sigma, bilateral_sigma);
            }
            display_image = filtered_display_image; // data is not copied
        }

        cv_image.image = display_image;
        cv_image.header.stamp = timestamp;
        intensity_estimate_pub_.publish(cv_image.toImageMsg());

        if (save_images_){

            static int image_counter = 0;
            std::string save_path = save_dir_ + "image" + std::to_string(image_counter) + ".png";
            cv::imwrite(save_path, display_image);
            image_counter++;
        }
    }

    void Comb_filter::convert_log_intensity_state_to_display_image(cv::Mat &image_out, const double &ts){

        constexpr double PERCENTAGE_PIXELS_TO_DISCARD = 0.5;
        double LOG_INTENSITY_OFFSET = std::log(1.5); // chosen because standard APS frames range from [1, 2].
        constexpr double FADE_DURATION = 2;                    // seconds. Time taken for dynamic range bounds to "take effect".
        // used for low-pass parameter to reach 95% of constant signal in FADE_DURATION seconds.
        double ALPHA = -std::log(1 - 0.95) / FADE_DURATION; // rad/s.
        constexpr double EXPECTED_MEAN = 0.5;

        static double t_last = 0.0;
        static double intensity_lower_bound = intensity_min_user_defined_;
        static double intensity_upper_bound = intensity_max_user_defined_;

        const double delta_t = ts - t_last;
        const double beta = std::exp(-delta_t * ALPHA); // low-pass parameter

        cv::Mat image;

        y0_.copyTo(image);             // ~[-0.5, 0.5]
        image += LOG_INTENSITY_OFFSET; // [-0.1, 0.9]
        cv::exp(image, image);         // ~[1, 2]
        image -= 1;                    // [0, 1]

        if (delta_t >= 0){

            if (adaptive_dynamic_range_){
                // VLOG(1) << "adaptive dynamic range";
                constexpr double MAX_INTENSITY_LOWER_BOUND = EXPECTED_MEAN - 0.2;
                constexpr double MIN_INTENSITY_UPPER_BOUND = EXPECTED_MEAN + 0.2;
                constexpr double EXTEND_RANGE = 0.05; // extend dynamic range for visual appeal.

                double robust_min, robust_max;
                minMaxLocRobust(image, &robust_min, &robust_max, PERCENTAGE_PIXELS_TO_DISCARD);

                //
                intensity_lower_bound = std::min(beta * intensity_lower_bound + (1 - beta) * (robust_min - EXTEND_RANGE), MAX_INTENSITY_LOWER_BOUND);

                intensity_upper_bound = std::max(beta * intensity_upper_bound + (1 - beta) * (robust_max + EXTEND_RANGE), MIN_INTENSITY_UPPER_BOUND);

                intensity_lower_bound = robust_min;
                intensity_upper_bound = robust_max;
            }
            else{
                VLOG(1) << "fixed dynamic range";
                intensity_lower_bound = beta * intensity_lower_bound + (1 - beta) * intensity_min_user_defined_;
                intensity_upper_bound = beta * intensity_upper_bound + (1 - beta) * intensity_max_user_defined_;
            }
        }
        const double intensity_range = intensity_upper_bound - intensity_lower_bound;
        image -= intensity_lower_bound;
        
        image.convertTo(image_out, CV_8UC1, 255.0 / intensity_range);
        // image.convertTo(image_out, CV_8UC1, 255.0);
        t_last = ts;
    }

    void Comb_filter::minMaxLocRobust(const cv::Mat& image, double* robust_min, double* robust_max, const double& percentage_pixels_to_discard){
        //   CHECK_NOTNULL(robust_max);
        //   CHECK_NOTNULL(robust_min);
        cv::Mat image_as_row;
        cv::Mat image_as_row_sorted;
        const int single_row_idx_min = (0.5*percentage_pixels_to_discard/100)*image.total();
        const int single_row_idx_max = (1 - 0.5*percentage_pixels_to_discard/100)*image.total();
        image_as_row = image.reshape(0, 1);
        cv::sort(image_as_row, image_as_row_sorted, CV_SORT_EVERY_ROW + CV_SORT_ASCENDING);
        image_as_row_sorted.convertTo(image_as_row_sorted, CV_64FC1);
        *robust_min = image_as_row_sorted.at<double>(single_row_idx_min);
        *robust_max = image_as_row_sorted.at<double>(single_row_idx_max);
    }   

    void Comb_filter::reconfigureCallback(comb::combConfig &config, uint32_t level){

        cutoff_frequency_global_ = config.Cutoff_frequency * 2 * M_PI;
        cutoff_frequency_per_event_component_ = config.Cutoff_frequency_per_event_component;
        contrast_threshold_on_user_defined_ = config.Contrast_threshold_ON;
        contrast_threshold_off_user_defined_ = config.Contrast_threshold_OFF;
        intensity_min_user_defined_ = config.Intensity_min;
        intensity_max_user_defined_ = config.Intensity_max;
        adaptive_contrast_threshold_ = config.Auto_detect_contrast_thresholds;
        spatial_filter_sigma_ = config.Spatial_filter_sigma;
        spatial_smoothing_method_ = int(config.Bilateral_filter);
        adaptive_dynamic_range_ = config.Auto_adjust_dynamic_range;
        color_image_ = config.Color_display;
    }

}