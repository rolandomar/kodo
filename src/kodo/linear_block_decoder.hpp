// Copyright Steinwurf ApS 2011-2012.
// Distributed under the "STEINWURF RESEARCH LICENSE 1.0".
// See accompanying file LICENSE.rst or
// http://www.steinwurf.com/licensing

#ifndef KODO_LINEAR_BLOCK_DECODER_HPP
#define KODO_LINEAR_BLOCK_DECODER_HPP

#include <stdint.h>

#include <boost/shared_ptr.hpp>
#include <boost/noncopyable.hpp>
#include <boost/make_shared.hpp>
#include <boost/optional.hpp>

#include <fifi/is_binary.hpp>

#include "linear_block_vector.hpp"

namespace kodo
{
    /// Implementes basic linear block decoder. The linear block decoder
    /// expects that an encoded symbol is described by a vector of coefficients.
    /// Using these coefficients the block decoder subtracts incomming symbols
    /// until the original data has been recreated.
    template<class SuperCoder>
    class linear_block_decoder : public SuperCoder
    {
    public:

        /// The field we use
        typedef typename SuperCoder::field_type field_type;

        /// The value_type used to store the field elements
        typedef typename field_type::value_type value_type;

        /// The coefficient vector
        typedef linear_block_vector<field_type> vector_type;

    public:

        /// Constructor
        linear_block_decoder()
            : m_rank(0),
              m_maximum_pivot(0)
            { }

        /// @copydoc final_coder_factory::construct()
        void construct(uint32_t max_symbols, uint32_t max_symbol_size)
            {
                SuperCoder::construct(max_symbols, max_symbol_size);

                m_uncoded.resize(max_symbols, false);
                m_coded.resize(max_symbols, false);
            }

        /// @copydoc final_coder_factory::initialize()
        void initialize(uint32_t symbols, uint32_t symbol_size)
            {
                SuperCoder::initialize(symbols, symbol_size);

                std::fill_n(m_uncoded.begin(), symbols, false);
                std::fill_n(m_coded.begin(), symbols, false);

                m_rank = 0;
                m_maximum_pivot = 0;
            }

        /// The decode function which consumes an incomming symbol and
        /// the corresponding symbol_id
        /// @param symbol_data the encoded symbol
        /// @param symbol_id the coefficients used to create the encoded symbol
        void decode(uint8_t *symbol_data, uint8_t *symbol_id)
            {
                assert(symbol_data != 0);
                assert(symbol_id != 0);

                value_type *symbol
                    = reinterpret_cast<value_type*>(symbol_data);

                value_type *vector
                    = reinterpret_cast<value_type*>(symbol_id);

                decode_with_vector(symbol, vector);
            }

        /// The decode function for systematic packets i.e.
        /// specific uncoded symbols.
        /// @param symbol_data the uncoded symbol
        /// @param symbol_index the index of this uncoded symbol
        void decode_raw(const uint8_t *symbol_data, uint32_t symbol_index)
            {
                assert(symbol_index < SuperCoder::symbols());
                assert(symbol_data != 0);

                if(m_uncoded[symbol_index])
                    return;

                const value_type *symbol
                    = reinterpret_cast<const value_type*>( symbol_data );

                if(m_coded[symbol_index])
                {
                    swap_decode(symbol, symbol_index);
                }
                else
                {
                    // Stores the symbol and updates the corresponding
                    // encoding vector
                    store_uncoded_symbol(symbol, symbol_index);

                    // Backwards substitution
                    value_type *vector = SuperCoder::vector(symbol_index);
                    backward_substitute(symbol, vector, symbol_index);

                    // We have increased the rank if we have finished the
                    // backwards substitution
                    ++m_rank;

                    m_uncoded[ symbol_index ] = true;

                    if(symbol_index > m_maximum_pivot)
                    {
                        m_maximum_pivot = symbol_index;
                    }

                }
            }

        /// @return true if the decoding is complete
        bool is_complete() const
            {
                return m_rank == SuperCoder::symbols();
            }

        /// @return the rank of the decoder
        uint32_t rank() const
            {
                return m_rank;
            }

        /// @param index the symbol index to check
        /// @return true if the symbol with the specified id
        ///         has already been received in the decoder
        bool symbol_exists(uint32_t index) const
            {
                assert(index < SuperCoder::symbols());
                return m_coded[ index ] || m_uncoded[ index ];
            }

    protected:

        /// Decodes a symbol based on the vector
        /// @param symbol_data buffer containing the encoding symbol
        /// @param symbol_id buffer containing the encoding vector
        void decode_with_vector(value_type *symbol_data, value_type *symbol_id)
            {
                assert(symbol_data != 0);
                assert(symbol_id != 0);

                // See if we can find a pivot
                boost::optional<uint32_t> pivot_index
                    = forward_substitute_to_pivot(symbol_data, symbol_id);

                if(!pivot_index)
                    return;

                if(!fifi::is_binary<field_type>::value)
                {
                    // Normalize symbol and vector
                    normalize(symbol_data, symbol_id, *pivot_index);
                }

                // Reduce the symbol further
                forward_substitute_from_pivot(symbol_data, symbol_id, *pivot_index);

                // Now with the found pivot reduce the existing symbols
                backward_substitute(symbol_data, symbol_id, *pivot_index);

                // Now save the received symbol
                store_coded_symbol(symbol_data, symbol_id, *pivot_index);

                // We have increased the rank
                ++m_rank;

                m_coded[ *pivot_index ] = true;

                if(*pivot_index > m_maximum_pivot)
                {
                    m_maximum_pivot = *pivot_index;
                }
            }

        /// When adding a raw symbol (i.e. uncoded) with a specific pivot id and
        /// the decoder already contains a coded symbol in that position this
        /// function performs the proper swap between the two symbols.
        /// @param symbol_data the data for the raw symbol
        /// @param pivot_index the pivot position of the raw symbol
        void swap_decode(const value_type *symbol_data, uint32_t pivot_index)
            {
                assert(m_coded[pivot_index] == true);
                assert(m_uncoded[pivot_index] == false);

                m_coded[pivot_index] = false;

                value_type *symbol_i = reinterpret_cast<value_type*>(SuperCoder::symbol(pivot_index));
                value_type *vector_i = SuperCoder::vector(pivot_index);

                value_type value =
                    vector_type::coefficient(pivot_index, vector_i);

                assert(value == 1);

                // Subtract the new pivot symbol
                vector_type::set_coefficient(pivot_index, vector_i, 0);

                SuperCoder::subtract(symbol_i, symbol_data,
                                     SuperCoder::symbol_length());

                // Now continue our new coded symbol we know that it must
                // if found it will contain a pivot id > that the current.
                decode_with_vector(symbol_i, vector_i);

                // The previous vector may still be in memory
                std::fill_n(vector_i, SuperCoder::vector_length(), 0);

                // Stores the symbol and sets the pivot in the vector
                store_uncoded_symbol(symbol_data, pivot_index);

                m_uncoded[pivot_index] = true;

                // No need to backwards substitute since we are
                // replacing an existing symbol. I.e. backwards
                // substitution must already have been done.
            }

        /// Iterates the encoding vector from where a pivot has been identified
        /// and subtracts existing symbols
        /// @param symbol_data the data of the encoded symbol
        /// @param symbol_id the data constituting the encoding vector
        /// @param pivot_index the index of the found pivot element
        void normalize(value_type *symbol_data,
                       value_type *symbol_id,
                       uint32_t pivot_index)
            {

                assert(symbol_id != 0);
                assert(symbol_data != 0);

                assert(pivot_index < SuperCoder::symbols());

                assert(m_uncoded[pivot_index] == false);
                assert(m_coded[pivot_index] == false);

                value_type coefficient =
                    vector_type::coefficient( pivot_index, symbol_id );

                assert(coefficient > 0);

                value_type inverted_coefficient = SuperCoder::invert(coefficient);

                // Update symbol and corresponding vector
                SuperCoder::multiply(symbol_id, inverted_coefficient,
                                     SuperCoder::vector_length());

                SuperCoder::multiply(symbol_data, inverted_coefficient,
                                     SuperCoder::symbol_length());

            }

        /// Iterates the encoding vector and subtracts existing symbols until
        /// a pivot element is found.
        /// @param symbol_data the data of the encoded symbol
        /// @param symbol_id the data constituting the encoding vector
        /// @return the pivot index if found.
        boost::optional<uint32_t> forward_substitute_to_pivot(
            value_type *symbol_data,
            value_type *symbol_id)
            {
                assert(symbol_id != 0);
                assert(symbol_data != 0);

                for(uint32_t i = 0; i < SuperCoder::symbols(); ++i)
                {

                    value_type current_coefficient
                        = vector_type::coefficient( i, symbol_id );

                    if( current_coefficient )
                    {
                        // If symbol exists
                        if( symbol_exists( i ) )
                        {
                            value_type *vector_i = SuperCoder::vector( i );
                            value_type *symbol_i =
                                reinterpret_cast<value_type*>(SuperCoder::symbol( i ));

                            if(fifi::is_binary<field_type>::value)
                            {
                                SuperCoder::subtract(
                                    symbol_id, vector_i,
                                    SuperCoder::vector_length());

                                SuperCoder::subtract(
                                    symbol_data, symbol_i,
                                    SuperCoder::symbol_length());
                            }
                            else
                            {
                                SuperCoder::multiply_subtract(
                                    symbol_id, vector_i,
                                    current_coefficient,
                                    SuperCoder::vector_length());

                                SuperCoder::multiply_subtract(
                                    symbol_data, symbol_i,
                                    current_coefficient,
                                    SuperCoder::symbol_length());
                            }
                        }
                        else
                        {
                            return boost::optional<uint32_t>( i );
                        }
                    }
                }

                return boost::none;
            }

        /// Iterates the encoding vector from where a pivot has been identified
        /// and subtracts existing symbols
        /// @param symbol_data the data of the encoded symbol
        /// @param symbol_id the data constituting the encoding vector
        /// @param pivot_index the index of the found pivot element
        void forward_substitute_from_pivot(value_type *symbol_data,
                                           value_type *symbol_id,
                                           uint32_t pivot_index)
            {
                // We have received an encoded symbol - described
                // by the symbol group. We now normalize the
                // the encoding vector according to the symbol id.
                // I.e. we make sure the pivot position has a "1"
                assert(symbol_id != 0);
                assert(symbol_data != 0);

                assert(pivot_index < SuperCoder::symbols());

                assert(m_uncoded[pivot_index] == false);
                assert(m_coded[pivot_index] == false);

                /// If this pivot was smaller than the maximum pivot we have
                /// we also need to potentially backward substitute the higher
                /// pivot values into the new packet
                for(uint32_t i = pivot_index + 1; i <= m_maximum_pivot; ++i)
                {
                    // Do we have a non-zero value here?
                    value_type value = vector_type::coefficient(i, symbol_id);

                    if( !value )
                    {
                        continue;
                    }

                    if( symbol_exists(i) )
                    {
                        value_type *vector_i = SuperCoder::vector(i);
                        value_type *symbol_i = reinterpret_cast<value_type*>(SuperCoder::symbol(i));

                        if(fifi::is_binary<field_type>::value)
                        {
                            SuperCoder::subtract(symbol_id, vector_i,
                                                 SuperCoder::vector_length());

                            SuperCoder::subtract(symbol_data, symbol_i,
                                                 SuperCoder::symbol_length());
                        }
                        else
                        {
                            SuperCoder::multiply_subtract(symbol_id, vector_i,
                                                          value,
                                                          SuperCoder::vector_length());

                            SuperCoder::multiply_subtract(symbol_data, symbol_i,
                                                          value,
                                                          SuperCoder::symbol_length());
                        }
                    }
                }
            }

        /// Backward substitute the found symbol into the
        /// existing symbols.
        /// @param symbol_data buffer containing the encoding symbol
        /// @param symbol_id buffer containing the encoding vector
        /// @param pivot_index the pivot index of the symbol in the
        ///        buffers symbol_id and symbol_data
        void backward_substitute(const value_type *symbol_data,
                                 const value_type *symbol_id,
                                 uint32_t pivot_index)
            {
                assert(symbol_id != 0);
                assert(symbol_data != 0);

                assert(pivot_index < SuperCoder::symbols());

                // We found a "1" that nobody else had as pivot, we now
                // substract this packet from other coded packets
                // - if they have a "1" on our pivot place
                for(uint32_t i = 0; i <= m_maximum_pivot; ++i)
                {
                    if( m_uncoded[i] )
                    {
                        // We know that we have no non-zero elements
                        // outside the pivot position.
                        continue;
                    }

                    if(i == pivot_index)
                    {
                        // We cannot backward substitute into ourself
                        continue;
                    }

                    if( m_coded[i] )
                    {
                        value_type *vector_i = SuperCoder::vector(i);

                        value_type value =
                            vector_type::coefficient( pivot_index, vector_i );

                        if( value )
                        {

                            value_type *symbol_i = reinterpret_cast<value_type*>(SuperCoder::symbol(i));

                            if(fifi::is_binary<field_type>::value)
                            {
                                SuperCoder::subtract(vector_i, symbol_id,
                                                     SuperCoder::vector_length());

                                SuperCoder::subtract(symbol_i, symbol_data,
                                                     SuperCoder::symbol_length());
                            }
                            else
                            {

                                // Update symbol and corresponding vector
                                SuperCoder::multiply_subtract(vector_i, symbol_id,
                                                              value,
                                                              SuperCoder::vector_length());

                                SuperCoder::multiply_subtract(symbol_i, symbol_data,
                                                              value,
                                                              SuperCoder::symbol_length());
                            }
                        }
                    }
                }
            }

        /// Store an encoded symbol and encoding vector with the specified
        /// pivot found.
        /// @param symbol_data buffer containing the encoding symbol
        /// @param symbol_id buffer containing the encoding vector
        /// @param pivot_index the pivot index
        void store_coded_symbol(const value_type *symbol_data,
                                const value_type *symbol_id,
                                uint32_t pivot_index)
            {
                assert(m_uncoded[pivot_index] == false);
                assert(m_coded[pivot_index] == false);
                assert(symbol_id != 0);
                assert(symbol_data != 0);

                // Copy it into the vector storage
                value_type *vector_dest = SuperCoder::vector( pivot_index );
                value_type *symbol_dest = reinterpret_cast<value_type*>(SuperCoder::symbol( pivot_index ));

                std::copy(symbol_id,
                          symbol_id + SuperCoder::vector_length(),
                          vector_dest);

                std::copy(symbol_data,
                          symbol_data + SuperCoder::symbol_length(),
                          symbol_dest);

            }

        /// Stores an uncoded or fully decoded symbol
        /// @param symbol_data the data for the symbol
        /// @param pivot_index the pivot index of the symbol
        void store_uncoded_symbol(const value_type *symbol_data,
                                  uint32_t pivot_index)
            {
                assert(symbol_data != 0);
                assert(m_uncoded[pivot_index] == false);
                assert(m_coded[pivot_index] == false);

                // Copy it into the symbol storage
                value_type *vector_dest = SuperCoder::vector( pivot_index );
                value_type *symbol_dest = reinterpret_cast<value_type*>(SuperCoder::symbol( pivot_index ));

                std::copy(symbol_data,
                          symbol_data + SuperCoder::symbol_length(),
                          symbol_dest);

                // Update the corresponding vector
                vector_type::set_coefficient(pivot_index, vector_dest, 1);

            }

    protected:

        /// The current rank of the decoder
        uint32_t m_rank;

        /// Stores the current maximum pivot index
        uint32_t m_maximum_pivot;

        /// Tracks whether a symbol is contained which
        /// is fully decoded
        std::vector<bool> m_uncoded;

        /// Tracks whether a symbol is partially decoded
        std::vector<bool> m_coded;
    };
}

#endif

