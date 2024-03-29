/*
    DISCLAIMER:
        The original author of this code is Tom Van Dijk. 
        The original code can be found in: https://github.com/tomvand
        Small modification have been performed to optimize the code and make it portable without ROS.
 */

#include <cmath>
#include <omp.h>
#include <opencv2/opencv.hpp>
#include <vector>

int filter = 1; // filters for outliers removal: Median - 0, Opening - 1, None - other value
int morph_elem = cv::MORPH_RECT;
int morph_size = 1; // no filtering if size = 0
int const max_operator = 4;
int num_threads = 4;

namespace {
    template <typename T>
    T bound(const T &val, const T &min, const T &max) {
        if (val < min)
            return min;
        if (val > max)
            return max;
        return val;
    }

    struct ImageParams {
        int width;
        int height;
        int ndisp;
        double f;      // Focal length of downscaled(!) image
        double f_disp; // Focal length for disparity calculation
        double B;
        int ymax; // Depths y > ymax are invalid
    };

    class LUT {
        public:
        LUT(ImageParams ip, double rv, double y_factor)
            : ip(ip), rv(rv), lut_x1(ip.width, std::vector<int>(ip.ndisp)),
              lut_x2(ip.width, std::vector<int>(ip.ndisp)), lut_y1(ip.height, std::vector<int>(ip.ndisp)),
              lut_y2(ip.height, std::vector<int>(ip.ndisp)), lut_dnew(ip.ndisp) {
            // For LUT generation, refer to Matthies et al., 2014 "Stereo vision..."
            // x1, x2 LUT
            for (int x = 0; x < ip.width; ++x) {
                for (int d = 1; d < ip.ndisp; ++d) { // Caution: d = 0 unhandled!
                    double cx = ip.width / 2.0;
                    double u = x - cx;
                    double zw = ip.f_disp * ip.B /
                                d; // Maybe test if zw < rv???
                    double xw = u * zw / ip.f;
                    double alpha = std::atan(zw / xw);
                    double arg = rv / std::sqrt(zw * zw + xw * xw);
                    double x1, x2;
                    if (arg > -1.0 && arg < 1.0) {
                        double alpha1 = std::asin(arg);
                        double r1x = zw / std::tan(alpha + alpha1);
                        double r2x = zw / std::tan(alpha - alpha1);
                        x1 = bound<double>(cx + ip.f * r1x / zw, 0, ip.width - 1);
                        x2 = bound<double>(cx + ip.f * r2x / zw, 0, ip.width - 1);
                    } else { // Camera is *inside* safety radius!
                        x1 = 0.0;
                        x2 = ip.width - 1;
                    }
                    lut_x1[x][d] =
                        static_cast<int>(x1);
                    lut_x2[x][d] = static_cast<int>(x2);
                }
            }
            // y1, y2 LUT
            for (int y = 0; y < ip.height; ++y) {
                for (int d = 1; d < ip.ndisp; ++d) { // Caution: d = 0 unhandled!
                    double cy = ip.height / 2.0;
                    double v = y - cy;
                    double zw = ip.f_disp * ip.B / d;
                    double yw = v * zw / ip.f;
                    double beta = std::atan(zw / yw);
                    double arg = rv / std::sqrt(zw * zw + yw * yw) * y_factor;
                    double y1, y2;
                    if (arg > -1.0 && arg < 1.0) {
                        double beta1 = std::asin(arg);
                        double r3y = zw / std::tan(beta + beta1);
                        double r4y = zw / std::tan(beta - beta1);
                        y1 = bound<double>(cy + ip.f * r3y / zw, 0, ip.height - 1);
                        y2 = bound<double>(cy + ip.f * r4y / zw, 0, ip.height - 1);
                    } else { // Camera is *inside* safety radius!
                        y1 = 0.0;
                        y2 = ip.height - 1;
                    }
                    lut_y1[y][d] = static_cast<int>(y1);
                    lut_y2[y][d] = static_cast<int>(y2);
                }
            }
            // dnew LUT
            for (int d = 1; d < ip.ndisp; ++d) { // Caution: d = 0 unhandled!
                double zw = ip.f_disp * ip.B / d;
                double znew = zw - rv;
                double dnew;
                if (znew > NEAR_CLIP) {
                    dnew = std::ceil(ip.f_disp * ip.B / znew);
                } else { // Camera is *inside* safety radius!
                    dnew = 999;
                }
                lut_dnew[d] = dnew;
            }
        }

        int x1(int x, int d) {
            assert(x >= 0 && x < ip.width && d > 0 && d < ip.ndisp);
            return lut_x1[x][d];
        }

        int x2(int x, int d) {
            assert(x >= 0 && x < ip.width && d > 0 && d < ip.ndisp);
            return lut_x2[x][d];
        }

        int y1(int y, int d) {
            assert(y >= 0 && y < ip.height && d > 0 && d < ip.ndisp);
            return lut_y1[y][d];
        }

        int y2(int y, int d) {
            assert(y >= 0 && y < ip.height && d > 0 && d < ip.ndisp);
            return lut_y2[y][d];
        }

        int dnew(int d) {
            assert(d > 0 && d < ip.ndisp);
            return lut_dnew[d];
        }

        private:
        std::vector<std::vector<int>> lut_x1;
        std::vector<std::vector<int>> lut_x2;
        std::vector<std::vector<int>> lut_y1;
        std::vector<std::vector<int>> lut_y2;
        std::vector<int> lut_dnew;
        ImageParams ip;
        double rv;
        const double NEAR_CLIP = 0.1;
    };

    class CSpaceExpander {
        public:
        CSpaceExpander(ImageParams ip, double rv, double y_factor) : ip(ip), lut(ip, rv, y_factor) {}

        void expand(const cv::Mat_<float> &disp, cv::Mat_<float> &cspace) {

            assert(disp.cols == ip.width && disp.rows == ip.height);
            cv::Mat_<float> temp;
            disp.copyTo(temp);
            // For C-Space expansion procedure, refer to Matthies et al., 2014
            // Row-wise expansion
            int y = 0, x = 0, d = 0, dnew = 0, x1 = 0, x2 = 0, x_write = 0, y1 = 0, y2 = 0, y_write = 0;
#pragma omp parallel for num_threads(num_threads) collapse(2) default(none) \
    shared(lut, disp, temp) private(y, x, d, x1, x2, x_write)
            for (y = 0; y < disp.rows; ++y) {
                for (x = 0; x < disp.cols; ++x) {
                    d = disp(y, x);
                    if (d > 0 && d < ip.ndisp) {
                        x1 = lut.x1(x, d);
                        x2 = lut.x2(x, d);
                        for (x_write = x1; x_write <= x2; ++x_write) {
                            if (!(d <= temp(y, x_write))) { // Negative test to handle NaNs!
                                temp(y, x_write) = d;
                            }
                        }
                    }
                }
            }
            // Column-wise expansion
            temp.copyTo(cspace);
#pragma omp parallel for num_threads(num_threads) collapse(2) default(none) \
    shared(lut, cspace, temp) private(y, x, d, y1, y2, y_write, dnew)
            for (x = 0; x < temp.cols; ++x) {
                for (y = 0; y < temp.rows; ++y) {
                    d = temp(y, x);
                    if (d > 0 && d < ip.ndisp) {
                        dnew = lut.dnew(d);
                        y1 = lut.y1(y, d);
                        y2 = lut.y2(y, d);
                        for (y_write = y1; y_write <= y2; ++y_write) {
                            if (!(dnew <= cspace(y_write, x))) { // Negative test to handle NaNs!
                                cspace(y_write, x) = dnew;
                            }
                        }
                    }
                }
            }
        }

        private:
        ImageParams ip;
        LUT lut;
    };

    void CSpaceProcess(cv::Mat_<float> &disp, cv::Mat_<float> &cspace, ImageParams ip, double rv, double y_factor) {
        CSpaceExpander ce(ip, rv, y_factor);
        // Bottom region of image is invalid
        disp(cv::Rect(0, ip.ymax + 1, disp.cols, disp.rows - ip.ymax - 1))
            .setTo(std::numeric_limits<float>::quiet_NaN());

        cv::Mat nan_mask = (disp != disp);
        disp.setTo(-1.0, nan_mask); // Set NaNs to < zero
        switch (filter) {
        case 0:
            cv::medianBlur(disp, disp, 5); // Median filtering to remove speckles
            break;
        case 1: {
            cv::Mat element = getStructuringElement(morph_elem, cv::Size(2 * morph_size + 1, 2 * morph_size + 1),
                                                    cv::Point(morph_size, morph_size));
            morphologyEx(disp, disp, cv::MORPH_OPEN,
                         element); // Apply the specified morphology operation. For closing: MORPH_CLOSE
            break;
        }
        default:
            std::cout << "No filter choise is made.\n";
        }

        disp.setTo(std::numeric_limits<float>::quiet_NaN(), nan_mask | (disp < 0.0)); // Put NaNs back

        // Expand disparity image
        ce.expand(disp, cspace);

        // Set bottom region to too close
        cspace(cv::Rect(0, ip.ymax + 1, disp.cols, disp.rows - ip.ymax - 1)).setTo(999);
    }
} // namespace

int main(int argc, char **argv) {
    // ros::init(argc, argv, "cspace");
    // ros::NodeHandle nh_private("~");
    cv::Mat_<float> disp, cspace;
    int color_mode = cv::IMREAD_GRAYSCALE;
    cv::Mat m = cv::imread(argv[1], color_mode);
    m.convertTo(disp, CV_32F);

    double rv = std::atof(argv[2]);
    rv = 0.3;

    double y_factor = std::atof(argv[3]);
    ImageParams ip = {
        .width = disp.cols,
        .height = disp.rows,
        .ndisp = 80,
        .f = 425 / 6, // Note: get from param for now, as camera_info does not arrive before construction...
        .f_disp = 425,
        .B = 0.6,
        .ymax = 350,
    };

    cv::Mat disp8, dispDilated8;
    cv::convertScaleAbs(disp, disp8);

    CSpaceProcess(disp, cspace, ip, rv, y_factor);

    cv::Mat cspace8;
    cv::Mat tmpDisplay;
    cv::convertScaleAbs(disp, tmpDisplay);
    cv::convertScaleAbs(cspace, cspace8);

    cv::imwrite("dispExpanded.png", cspace8);
    // cv::waitKey(0);
}
