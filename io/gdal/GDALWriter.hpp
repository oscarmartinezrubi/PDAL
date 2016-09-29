/******************************************************************************
* Copyright (c) 2016, Hobu Inc. (info@hobu.co)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <pdal/PointView.hpp>
#include <pdal/Writer.hpp>
#include <pdal/plugin.hpp>

extern "C" int32_t GDALWriter_ExitFunc();
extern "C" PF_ExitFunc GDALWriter_InitPlugin();

namespace pdal
{

class Grid
{
public:
    static const int statCount = 1;
    static const int statMin = 2;
    static const int statMax = 4;
    static const int statMean = 8;
    static const int statStdDev = 16;
    static const int statIdw = 32;

    Grid(size_t width, size_t height, double edgeLength, double radius,
            double noData, int outputTypes, size_t windowSize) :
        m_width(width), m_height(height), m_windowSize(windowSize),
        m_edgeLength(edgeLength), m_radius(radius), m_noData(noData),
        m_outputTypes(outputTypes)
    {
        size_t size(width * height);

        m_count.reset(new DataVec(size));
        if (m_outputTypes & statMin)
            m_min.reset(new DataVec(size,
                std::numeric_limits<double>::max()));
        if (m_outputTypes & statMax)
            m_max.reset(new DataVec(size,
                std::numeric_limits<double>::lowest()));
        if (m_outputTypes & statIdw)
        {
            m_idw.reset(new DataVec(size));
            m_idwDist.reset(new DataVec(size));
        }
        if ((m_outputTypes & statMean) || (m_outputTypes & statStdDev))
            m_mean.reset(new DataVec(size));
        if (m_outputTypes & statStdDev)
            m_stdDev.reset(new DataVec(size));
    }

    // Find an index into the actual storage given a grid coordinate.
    size_t index(size_t i, size_t j)
        { return (j * m_width) + i; }

    bool empty(size_t i, size_t j)
        { return empty(index(i, j)); }

    bool empty(size_t idx)
        { return ((*m_count)[idx] <= 0); }

    int horizontalIndex(double x)
        { return x / m_edgeLength; }

    int verticalIndex(double y)
        { return m_height - (y / m_edgeLength) - 1; }

    double horizontalPos(size_t i)
        { return (i + .5) * m_edgeLength; }

    double verticalPos(size_t j)
        { return (m_height - (j + .5)) * m_edgeLength; }

    double distance(size_t i, size_t j, double x, double y)
    {
        double x1 = horizontalPos(i);
        double y1 = verticalPos(j);
        return sqrt(pow(x1 - x, 2) + pow(y1 - y, 2));
    }

    int numBands() const
    {
        int num = 0;

        if (m_outputTypes & statCount)
            num++;
        if (m_outputTypes & statMin)
            num++;
        if (m_outputTypes & statMax)
            num++;
        if (m_outputTypes & statMean)
            num++;
        if (m_outputTypes & statIdw)
            num++;
        if (m_outputTypes & statStdDev)
            num++;
        return num;
    }

    uint8_t *data(const std::string& name)
    {
        if (name == "count")
            return (m_outputTypes & statCount ?
                (uint8_t *)m_count->data() : nullptr);
        if (name == "min")
            return (m_outputTypes & statMin ?
                (uint8_t *)m_min->data() : nullptr);
        if (name == "max")
            return (m_outputTypes & statMax ?
                (uint8_t *)m_max->data() : nullptr);
        if (name == "mean")
            return (m_outputTypes & statMean ?
                (uint8_t *)m_mean->data() : nullptr);
        if (name == "idw")
            return (m_outputTypes & statIdw ?
                (uint8_t *)m_idw->data() : nullptr);
        if (name == "stdev")
            return (m_outputTypes & statStdDev ?
                (uint8_t *)m_stdDev->data() : nullptr);
        return nullptr;
    }

    void addPoint(double x, double y, double z)
    {
        int iOrigin = horizontalIndex(x);
        int jOrigin = verticalIndex(y);

        // First quadrant;
        int i = iOrigin + 1;
        int j = jOrigin;
        while (i < (int)m_width && j >= 0)
        {
            double d = distance(i, j, x, y);
            if (d < m_radius)
            {
                update(i, j, z, d);
                i++;
            }
            else
            {
                if (i == iOrigin + 1)
                    break;
                i = iOrigin + 1;
                j--;
            }
        }

        // Second quadrant;
        i = iOrigin;
        j = jOrigin - 1;
        while (i >= 0 && j >= 0)
        {
            double d = distance(i, j, x, y);
            if (d < m_radius)
            {
                update(i, j, z, d);
                j--;
            }
            else
            {
                if (j == jOrigin - 1)
                    break;
                j = jOrigin - 1;
                i--;
            }
        }

        // Third quadrant;
        i = iOrigin - 1;
        j = jOrigin;
        while (i >= 0 && j < (int)m_height)
        {
            double d = distance(i, j, x, y);
            if (d < m_radius)
            {
                update(i, j, z, d);
                i--;
            }
            else
            {
                if (i == iOrigin - 1)
                    break;
                i = iOrigin - 1;
                j++;
            }
        }

        // Fourth quadrant;
        i = iOrigin;
        j = jOrigin + 1;
        while (i < (int)m_width && j < (int)m_height)
        {
            double d = distance(i, j, x, y);
            if (d < m_radius)
            {
                update(i, j, z, d);
                j++;
            }
            else
            {
                if (j == jOrigin + 1)
                    break;
                j = jOrigin + 1;
                i++;
            }
        }

        // This is a questionable case.  If a point is in a cell, shouldn't
        // it just be counted?
        double d = distance(iOrigin, jOrigin, x, y);
        if (d < m_radius)
            update(iOrigin, jOrigin, z, d);
    }

    void update(int i, int j, double val, double dist)
    {
        // See
        // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
        // https://en.wikipedia.org/wiki/Inverse_distance_weighting

        size_t offset = index(i, j);

        double& count = (*m_count)[offset];
        count++;

        if (m_min)
        {
            double& min = (*m_min)[offset];
            min = std::min(val, min);
        }

        if (m_max)
        {
            double& max = (*m_max)[offset];
            max = std::max(val, max);
        }

        if (m_mean)
        {
            double& mean = (*m_mean)[offset];
            double delta = val - mean;
            if (m_stdDev)
            {
                mean += delta / count;

                double& stdDev = (*m_stdDev)[offset];
                stdDev += delta * (val - mean);
            }
        }

        if (m_idw)
        {
            double& idw = (*m_idw)[offset];
            idw += val / dist;

            double& idwDist = (*m_idwDist)[offset];
            idwDist += 1 / dist;
        }
    }

    void finalize()
    {
        // See
        // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
        // https://en.wikipedia.org/wiki/Inverse_distance_weighting
        if (m_stdDev)
            for (size_t i = 0; i < m_count->size(); ++i)
                if (!empty(i))
                    (*m_stdDev)[i] = sqrt((*m_stdDev)[i] / (*m_count)[i]);

        if (m_idw)
            for (size_t i = 0; i < m_count->size(); ++i)
                if (!empty(i))
                    (*m_idw)[i] /= (*m_idwDist)[i];

        if (m_windowSize > 0)
            windowFill();
        else
            for (size_t i = 0; i < m_count->size(); ++i)
                if (empty(i))
                    fillNodata(i);
        
    }

    void fillNodata(size_t i)
    {
        if (m_min)
            (*m_min)[i] = m_noData;
        if (m_max)
            (*m_max)[i] = m_noData;
        if (m_mean)
            (*m_mean)[i] = m_noData;
        if (m_idw)
            (*m_idw)[i] = m_noData;
        if (m_stdDev)
            (*m_stdDev)[i] = m_noData;
    }

    // This is a last resort value-filling algorithm that does inverse-distance
    // weighting of the values around an empty cell.
    void windowFill()
    {
        for (size_t i = 0; i < width(); ++i)
            for (size_t j = 0; j < height(); ++j)
                if (empty(i, j))
                    windowFill(i, j);
    }

    void windowFill(size_t dstI, size_t dstJ)
    {
        size_t istart = std::max(0UL, dstI - m_windowSize);
        size_t iend = std::min(width(), dstI + m_windowSize + 1);
        size_t jstart = std::max(0UL, dstJ - m_windowSize);
        size_t jend = std::min(height(), dstJ + m_windowSize + 1);

        double distSum = 0;
        size_t dstIdx = index(dstI, dstJ);

        // Initialize to 0 (rather than numeric_limits::max/lowest) since we're
        // going to accumulate and average.
        if (m_min)
            (*m_min)[dstIdx] = 0;
        if (m_max)
            (*m_max)[dstIdx] = 0;

        for (size_t i = istart; i < iend; ++i)
            for (size_t j = jstart; j < jend; ++j)
            {
                size_t srcIdx = index(i, j);
                if (i == j || empty(srcIdx))
                    continue;
                // The ternaries just avoid underflow UB.  We're just trying to
                // find the distance from j to dstJ or i to dstI.
                double distance = std::max(j > dstJ ? j - dstJ : dstJ - j,
                                           i > dstI ? i - dstJ : dstJ - i);
                windowFillCell(srcIdx, dstIdx, distance);
                distSum += (1 / distance);
            }

        // Divide summed values by the (inverse) distance sum.
        if (distSum > 0)
        {
            if (m_min)
                (*m_min)[dstIdx] /= distSum;
            if (m_max)
                (*m_max)[dstIdx] /= distSum;
            if (m_mean)
                (*m_mean)[dstIdx] /= distSum;
            if (m_idw)
                (*m_idw)[dstIdx] /= distSum;
            if (m_stdDev)
                (*m_stdDev)[dstIdx] /= distSum;
        }
        else
            fillNodata(dstIdx);
    }

    void windowFillCell(size_t srcIdx, size_t dstIdx, double distance)
    {
        if (m_min)
            (*m_min)[dstIdx] += (*m_min)[srcIdx] / distance;
        if (m_max)
            (*m_max)[dstIdx] += (*m_max)[srcIdx] / distance;
        if (m_mean)
            (*m_mean)[dstIdx] += (*m_mean)[srcIdx] / distance;
        if (m_idw)
            (*m_idw)[dstIdx] += (*m_idw)[srcIdx] / distance;
        if (m_stdDev)
            (*m_stdDev)[dstIdx] += (*m_stdDev)[srcIdx] / distance;
    }

    size_t width() const
        { return m_width; }
    size_t height() const
        { return m_height; }
    double noData() const
        { return m_noData; }

private:
    size_t m_width;
    size_t m_height;
    size_t m_windowSize;
    double m_edgeLength;
    double m_radius;
    double m_noData;

    typedef std::vector<double> DataVec;
    typedef std::unique_ptr<DataVec> DataPtr;
    DataPtr m_count;
    DataPtr m_min;
    DataPtr m_max;
    DataPtr m_mean;
    DataPtr m_stdDev;
    DataPtr m_idw;
    DataPtr m_idwDist;

    int m_outputTypes;
};
typedef std::unique_ptr<Grid> GridPtr;

class PDAL_DLL GDALWriter : public Writer
{
public:
    static void * create();
    static int32_t destroy(void *);
    std::string getName() const;

private:
    virtual void addArgs(ProgramArgs& args);
    virtual void initialize();
    virtual void ready(PointTableRef table);
    virtual void write(const PointViewPtr data);
    virtual void done(PointTableRef table);

    std::string m_drivername;
    BOX2D m_bounds;
    double m_edgeLength;
    double m_radius;
    StringList m_options;
    StringList m_outputTypeString;
    size_t m_windowSize;
    int m_outputTypes;
    GridPtr m_grid;
};

}
