/*!
 * \file complex_carrier_test.cc
 * \brief  This file implements tests for the generation of complex exponentials.
 * \author Carles Fernandez-Prades, 2014. cfernandez(at)cttc.es
 *
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2015  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */


#include <complex>
#include <ctime>
#include <armadillo>
#include <volk/volk.h>
#include "gnss_signal_processing.h"

DEFINE_int32(size_carrier_test, 100000, "Size of the arrays used for complex carrier testing");


TEST(ComplexCarrier_Test, StandardComplexImplementation)
{
    // Dynamic allocation creates new usable space on the program STACK
    // (an area of RAM specifically allocated to the program)
    std::complex<float>* output = new std::complex<float>[FLAGS_size_carrier_test];
    const double _f = 2000;
    const double _fs = 2000000;
    const double phase_step = (double)((GPS_TWO_PI * _f) / _fs);
    double phase = 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long int begin = tv.tv_sec * 1000000 + tv.tv_usec;

    for(int i = 0; i < FLAGS_size_carrier_test; i++)
         {
             output[i] = std::complex<float>(cos(phase), sin(phase));
             phase += phase_step;
         }

    gettimeofday(&tv, NULL);
    long long int end = tv.tv_sec * 1000000 + tv.tv_usec;
    std::cout << "A " << FLAGS_size_carrier_test
              << "-length complex carrier in standard C++ (dynamic allocation) generated in " << (end - begin)
              << " microseconds" << std::endl;

    std::complex<float> expected(1,0);
    std::vector<std::complex<float>> mag(FLAGS_size_carrier_test);
    for(int i = 0; i < FLAGS_size_carrier_test; i++)
        {
            mag[i] = output[i] * std::conj(output[i]);
        }
    delete[] output;
    for(int i = 0; i < FLAGS_size_carrier_test; i++)
        {
            ASSERT_FLOAT_EQ(std::norm(expected), std::norm(mag[i]));
        }

    ASSERT_LE(0, end - begin);
}


TEST(ComplexCarrier_Test, C11ComplexImplementation)
{
    // declaration: load data onto the program data segment
    std::vector<std::complex<float>> output(FLAGS_size_carrier_test);
    const double _f = 2000;
    const double _fs = 2000000;
    const double phase_step = (double)((GPS_TWO_PI * _f) / _fs);
    double phase = 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long int begin = tv.tv_sec * 1000000 + tv.tv_usec;
    for (int i = 0; i < FLAGS_size_carrier_test; i++)
        {
            output[i] = std::complex<float>(cos(phase), sin(phase));
            phase += phase_step;
        }
    gettimeofday(&tv, NULL);
    long long int end = tv.tv_sec * 1000000 + tv.tv_usec;
    std::cout << "A " << FLAGS_size_carrier_test
              << "-length complex carrier in standard C++ (declaration) generated in " << (end - begin)
              << " microseconds" << std::endl;
    ASSERT_LE(0, end - begin);
    std::complex<float> expected(1,0);
    std::vector<std::complex<float>> mag(FLAGS_size_carrier_test);
    for(int i = 0; i < FLAGS_size_carrier_test; i++)
        {
            mag[i] = output[i] * std::conj(output[i]);
            ASSERT_FLOAT_EQ(std::norm(expected), std::norm(mag[i]));
        }
}




TEST(ComplexCarrier_Test, OwnComplexImplementation)
{
    std::complex<float>* output = new std::complex<float>[FLAGS_size_carrier_test];
    double _f = 2000;
    double _fs = 2000000;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long int begin = tv.tv_sec * 1000000 + tv.tv_usec;

    complex_exp_gen(output, _f, _fs, (unsigned int)FLAGS_size_carrier_test);

    gettimeofday(&tv, NULL);
    long long int end = tv.tv_sec * 1000000 + tv.tv_usec;
    std::cout << "A " << FLAGS_size_carrier_test
              << "-length complex carrier using fixed point generated in " << (end - begin)
              << " microseconds" << std::endl;

    std::complex<float> expected(1,0);
    std::vector<std::complex<float>> mag(FLAGS_size_carrier_test);
    for(int i = 0; i < FLAGS_size_carrier_test; i++)
        {
            mag[i] = output[i] * std::conj(output[i]);
        }
    delete[] output;
    for(int i = 0; i < FLAGS_size_carrier_test; i++)
        {
            ASSERT_NEAR(std::norm(expected), std::norm(mag[i]), 0.0001);
        }
    ASSERT_LE(0, end - begin);
}
