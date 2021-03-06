/*!
 * \file conjugate_cc.h
 * \brief Conjugate a stream of gr_complex
 * \author Carles Fernandez Prades, cfernandez(at)cttc.es
 *
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#ifndef GNSS_SDR_CONJUGATE_CC_H
#define GNSS_SDR_CONJUGATE_CC_H

#if GNURADIO_USES_STD_POINTERS
#include <memory>
#else
#include <boost/shared_ptr.hpp>
#endif
#include <gnuradio/sync_block.h>
#include <gnuradio/types.h>  // for gr_vector_const_void_star

class conjugate_cc;

#if GNURADIO_USES_STD_POINTERS
using conjugate_cc_sptr = std::shared_ptr<conjugate_cc>;
#else
using conjugate_cc_sptr = boost::shared_ptr<conjugate_cc>;
#endif

conjugate_cc_sptr make_conjugate_cc();

/*!
 * \brief This class adapts a std::complex<short> stream
 * into two 32-bits (float) streams
 */
class conjugate_cc : public gr::sync_block
{
public:
    int work(int noutput_items,
        gr_vector_const_void_star &input_items,
        gr_vector_void_star &output_items);

private:
    friend conjugate_cc_sptr make_conjugate_cc();
    conjugate_cc();
};

#endif
