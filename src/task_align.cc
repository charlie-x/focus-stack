#include "task_align.hh"
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/video.hpp>
#include <opencv2/core/utility.hpp>
#include <cmath>
#include <cstdio>

using namespace focusstack;

static inline float sq(float x) { return x * x; }

Task_Align::Task_Align(std::shared_ptr<ImgTask> refgray, std::shared_ptr<ImgTask> refcolor,
                       std::shared_ptr<ImgTask> srcgray, std::shared_ptr<ImgTask> srccolor,
                       std::shared_ptr<Task_Align> initial_guess,
                       std::shared_ptr<Task_LoadImg> cropinfo,
                       FocusStack::align_flags_t flags)
{
  m_filename = "aligned_" + srccolor->basename();
  m_name = "Align " + srccolor->basename() + " to " + refcolor->basename();

  m_refgray = refgray;
  m_refcolor = refcolor;
  m_srcgray = srcgray;
  m_srccolor = srccolor;
  m_initial_guess = initial_guess;
  m_cropinfo = cropinfo;
  m_flags = flags;

  m_depends_on.push_back(refgray);
  m_depends_on.push_back(refcolor);
  m_depends_on.push_back(srcgray);
  m_depends_on.push_back(srccolor);
  if (initial_guess) m_depends_on.push_back(initial_guess);
  if (cropinfo) m_depends_on.push_back(cropinfo);

  // Create initial guess for the transformation
  m_transformation.create(2, 3, CV_32F);
  m_transformation = 0;
  m_transformation.at<float>(0, 0) = 1.0f;
  m_transformation.at<float>(1, 1) = 1.0f;

  // For contrast; it is a column vector of [constant, x, x^2, y, y^2] factors
  m_contrast.create(5, 1, CV_32F);
  m_contrast = 0.0f;
  m_contrast.at<float>(0, 0) = 1.0f;

  // For white balance, it is column vector of [bb, bc, gb, gc, rb, rc]
  // brightness & contrast terms for each channel.
  m_whitebalance.create(6, 1, CV_32F);
  m_whitebalance = 0.0f;
  m_whitebalance.at<float>(1, 0) = 1.0f;
  m_whitebalance.at<float>(3, 0) = 1.0f;
  m_whitebalance.at<float>(5, 0) = 1.0f;
}

void Task_Align::task()
{
  if (m_refcolor == m_srccolor)
  {
    m_result = m_srccolor->img();
  }
  else
  {
    if (m_initial_guess)
    {
      m_initial_guess->m_transformation.copyTo(m_transformation);
    }

    if (m_cropinfo)
    {
      // Mask off the reflected borders generated by Task_LoadImg.
      int expand_x = m_cropinfo->img().cols - m_cropinfo->orig_size().width;
      int expand_y = m_cropinfo->img().rows - m_cropinfo->orig_size().height;
      m_roi = cv::Rect(expand_x / 2, expand_y / 2,
                       m_cropinfo->img().cols - expand_x,
                       m_cropinfo->img().rows - expand_y);
    }
    else
    {
      m_roi = cv::Rect(0, 0, m_srccolor->img().cols, m_srccolor->img().rows);
    }

    match_transform(256, true);

    if (!(m_flags & FocusStack::ALIGN_NO_CONTRAST))
    {
      match_contrast();
    }

    if (!(m_flags & FocusStack::ALIGN_NO_WHITEBALANCE))
    {
      match_whitebalance();
    }

    if (m_flags & FocusStack::ALIGN_FULL_RESOLUTION)
    {
      int res = std::max(m_srccolor->img().cols, m_srccolor->img().rows);
      match_transform(res, false);
    }
    else
    {
      // By default limit image resolution used in alignment to 2k.
      // Because this uses subpixel positioning, higher resolution provides little benefit.
      match_transform(2048, false);
    }

    apply_transform(m_srccolor->img(), m_result, false);

    if (!(m_flags & FocusStack::ALIGN_NO_CONTRAST) || !(m_flags & FocusStack::ALIGN_NO_WHITEBALANCE))
    {
      apply_contrast_whitebalance(m_result);
    }
  }

  m_refgray.reset();
  m_refcolor.reset();
  m_srcgray.reset();
  m_srccolor.reset();
  m_initial_guess.reset();
  m_cropinfo.reset();
}

// Collect samples and use them to predict contrast between images
// based on 5 factors: constant difference, x, x^2, y and y^2 dependencies.
// These factors can model most lighting differences caused by e.g.
// rolling shutter and lens vignetting.
void Task_Align::match_contrast()
{
  cv::Mat ref, src;

  int xsamples = 64;
  int ysamples = 64;
  int total = xsamples * ysamples;

  cv::Mat tmp;
  apply_transform(m_srcgray->img(), tmp, false);

  cv::resize(m_refgray->img()(m_roi), ref, cv::Size(xsamples, ysamples), 0, 0, cv::INTER_AREA);
  cv::resize(tmp(m_roi), src, cv::Size(xsamples, ysamples), 0, 0, cv::INTER_AREA);

  cv::Mat contrast(total, 1, CV_32F);
  cv::Mat positions(total, 5, CV_32F);

  for (int y = 0; y < ysamples; y++)
  {
    for (int x = 0; x < xsamples; x++)
    {
      int idx = y * xsamples + x;

      float yd = (y - ref.rows/2.0f) / (float)ref.rows;
      float xd = (x - ref.cols/2.0f) / (float)ref.cols;

      float c = (float)ref.at<uint8_t>(y, x) / (float)src.at<uint8_t>(y, x);

      contrast.at<float>(idx) = c;
      positions.at<float>(idx, 0) = 1.0f;
      positions.at<float>(idx, 1) = xd;
      positions.at<float>(idx, 2) = sq(xd);
      positions.at<float>(idx, 3) = yd;
      positions.at<float>(idx, 4) = sq(yd);
    }
  }

  cv::solve(positions, contrast, m_contrast, cv::DECOMP_SVD);

  if (m_verbose)
  {
    std::string name = basename();
    std::printf("%s contrast map: C:%0.3f, X:%0.3f, X2:%0.3f, Y:%0.3f, Y2:%0.3f\n",
                name.c_str(),
                m_contrast.at<float>(0), m_contrast.at<float>(1), m_contrast.at<float>(2),
                m_contrast.at<float>(3), m_contrast.at<float>(4));
  }
}

void Task_Align::match_transform(int max_resolution, bool rough)
{
  cv::Mat ref, src, mask;

  int resolution = std::max(m_refgray->img().cols, m_refgray->img().rows);
  float scale_ratio = 1.0f;

  if (resolution <= max_resolution)
  {
    ref = m_refgray->img();
    m_srcgray->img().copyTo(src);
  }
  else
  {
    scale_ratio = max_resolution / (float)resolution;
    cv::resize(m_refgray->img(), ref, cv::Size(), scale_ratio, scale_ratio, cv::INTER_AREA);
    cv::resize(m_srcgray->img(), src, cv::Size(), scale_ratio, scale_ratio, cv::INTER_AREA);
  }

  mask.create(ref.rows, ref.cols, CV_8U);
  mask = 0;
  mask(cv::Rect(m_roi.x * scale_ratio, m_roi.y * scale_ratio, m_roi.width * scale_ratio, m_roi.height * scale_ratio)) = 255;

  apply_contrast_whitebalance(src);

  m_transformation.at<float>(0, 2) *= scale_ratio;
  m_transformation.at<float>(1, 2) *= scale_ratio;

  if (rough)
  {
    cv::findTransformECC(ref, src, m_transformation, cv::MOTION_AFFINE,
                        cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 25, 0.01),
                        mask);
  }
  else
  {
    cv::findTransformECC(ref, src, m_transformation, cv::MOTION_AFFINE,
                        cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 50, 0.001),
                        mask);
  }


  m_transformation.at<float>(0, 2) /= scale_ratio;
  m_transformation.at<float>(1, 2) /= scale_ratio;

  if (m_verbose)
  {
    std::string name = basename();
    std::printf("%s %s transform: [%0.3f %0.3f %0.3f; %0.3f %0.3f %0.3f; %0.3f %0.3f %0.3f]\n",
                name.c_str(), rough ? "rough" : "final",
                m_transformation.at<float>(0, 0), m_transformation.at<float>(0, 1), m_transformation.at<float>(0, 2),
                m_transformation.at<float>(1, 0), m_transformation.at<float>(1, 1), m_transformation.at<float>(1, 2),
                m_transformation.at<float>(2, 0), m_transformation.at<float>(2, 1), m_transformation.at<float>(2, 2));
  }
}

void Task_Align::match_whitebalance()
{
  cv::Mat ref, src;

  int xsamples = 64;
  int ysamples = 64;
  int total = xsamples * ysamples;

  cv::Mat tmp;
  apply_transform(m_srccolor->img(), tmp, false);
  apply_contrast_whitebalance(tmp);

  cv::resize(m_refcolor->img()(m_roi), ref, cv::Size(xsamples, ysamples), 0, 0, cv::INTER_AREA);
  cv::resize(tmp(m_roi), src, cv::Size(xsamples, ysamples), 0, 0, cv::INTER_AREA);

  cv::Mat targets(total * 3, 1, CV_32F);
  cv::Mat factors(total * 3, 6, CV_32F);
  factors = 0.0f;

  for (int y = 0; y < ysamples; y++)
  {
    for (int x = 0; x < xsamples; x++)
    {
      int idx = y * xsamples + x;

      cv::Vec3b srcpixel = src.at<cv::Vec3b>(y, x);
      cv::Vec3b refpixel = ref.at<cv::Vec3b>(y, x);

      targets.at<float>(idx * 3 + 0, 0) = refpixel[0];
      targets.at<float>(idx * 3 + 1, 0) = refpixel[1];
      targets.at<float>(idx * 3 + 2, 0) = refpixel[2];

      factors.at<float>(idx * 3 + 0, 0) = 1.0f;
      factors.at<float>(idx * 3 + 0, 1) = srcpixel[0];
      factors.at<float>(idx * 3 + 1, 2) = 1.0f;
      factors.at<float>(idx * 3 + 1, 3) = srcpixel[1];
      factors.at<float>(idx * 3 + 2, 4) = 1.0f;
      factors.at<float>(idx * 3 + 2, 5) = srcpixel[2];
    }
  }

  cv::solve(factors, targets, m_whitebalance, cv::DECOMP_SVD);

  if (m_verbose)
  {
    std::string name = basename();
    std::printf("%s whitebalance: R:x%0.3f%+0.1f, G:x%0.3f%+0.1f, B:x%0.3f%+0.1f\n",
                name.c_str(),
                m_whitebalance.at<float>(5), m_whitebalance.at<float>(4),
                m_whitebalance.at<float>(3), m_whitebalance.at<float>(2),
                m_whitebalance.at<float>(1), m_whitebalance.at<float>(0));
  }
}

void Task_Align::apply_contrast_whitebalance(cv::Mat& img)
{
  if (img.channels() == 1)
  {
    // For grayscale images, apply contrast only
    float delta = 0.0f;

    for (int y = 0; y < img.rows; y++)
    {
      for (int x = 0; x < img.cols; x++)
      {
        float yd = (y - img.rows/2.0f) / (float)img.rows;
        float xd = (x - img.cols/2.0f) / (float)img.cols;

        float c = m_contrast.at<float>(0)
                + xd * (m_contrast.at<float>(1) + m_contrast.at<float>(2) * xd)
                + yd * (m_contrast.at<float>(3) + m_contrast.at<float>(4) * yd);

        // Simple dithering reduces banding in result image
        uint8_t v = img.at<uint8_t>(y, x);
        float f = v * c;
        v = std::min(255, std::max(0, (int)(f + delta)));
        delta += f - v;
        img.at<uint8_t>(y, x) = v;
      }
    }
  }
  else
  {
    // For RGB images, apply contrast and white balance

    float delta[3] = {0.0f, 0.0f, 0.0f};
    for (int y = 0; y < img.rows; y++)
    {
      for (int x = 0; x < img.cols; x++)
      {
        float yd = (y - img.rows/2.0f) / (float)img.rows;
        float xd = (x - img.cols/2.0f) / (float)img.cols;

        float c = m_contrast.at<float>(0)
                + xd * (m_contrast.at<float>(1) + m_contrast.at<float>(2) * xd)
                + yd * (m_contrast.at<float>(3) + m_contrast.at<float>(4) * yd);


        cv::Vec3b v = img.at<cv::Vec3b>(y, x);
        float b = v[0] * c * m_whitebalance.at<float>(1) + m_whitebalance.at<float>(0);
        float g = v[1] * c * m_whitebalance.at<float>(3) + m_whitebalance.at<float>(2);
        float r = v[2] * c * m_whitebalance.at<float>(5) + m_whitebalance.at<float>(4);
        v[0] = std::min(255, std::max(0, (int)(b + delta[0])));
        v[1] = std::min(255, std::max(0, (int)(g + delta[1])));
        v[2] = std::min(255, std::max(0, (int)(r + delta[2])));
        delta[0] += b - v[0];
        delta[1] += g - v[1];
        delta[2] += r - v[2];
        img.at<cv::Vec3b>(y, x) = v;
      }
    }
  }
}

void Task_Align::apply_transform(const cv::Mat &src, cv::Mat &dst, bool inverse)
{
  int invflag = (!inverse) ? cv::WARP_INVERSE_MAP : 0;

  cv::warpAffine(src, dst, m_transformation, cv::Size(src.cols, src.rows), cv::INTER_CUBIC | invflag, cv::BORDER_REFLECT);
}
