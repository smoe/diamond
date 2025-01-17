/****
DIAMOND protein aligner
Copyright (C) 2013-2018 Benjamin Buchfink <buchfink@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
****/

#include <algorithm>
#include <thread>
#include "../dp.h"
#include "swipe_matrix.h"
#include "swipe.h"
#include "target_iterator.h"
#include "../../util/thread.h"
#include "../../util/data_structures/mem_buffer.h"

using namespace std;

template<typename _sv>
struct Banded3FrameSwipeMatrix
{

	struct ColumnIterator
	{
		ColumnIterator(_sv* hgap_front, _sv* score_front) :
			hgap_ptr_(hgap_front),
			score_ptr_(score_front)
		{
			sm4 = ScoreTraits<_sv>::zero();
			sm3 = *score_ptr_;
			sm2 = *(score_ptr_ + 1);
		}
		inline void operator++()
		{
			++hgap_ptr_; ++score_ptr_;
			sm4 = sm3;
			sm3 = sm2;
			sm2 = *(score_ptr_ + 1);
		}
		inline _sv hgap() const
		{
			return *(hgap_ptr_ + 3);
		}
		inline void set_hgap(const _sv& x)
		{
			*hgap_ptr_ = x;
		}
		inline void set_score(const _sv& x)
		{
			*score_ptr_ = x;
		}
		void set_zero()
		{
			*(score_ptr_ - 1) = ScoreTraits<_sv>::zero();
			*(score_ptr_ - 2) = ScoreTraits<_sv>::zero();
			*(score_ptr_ - 3) = ScoreTraits<_sv>::zero();
		}
		_sv *hgap_ptr_, *score_ptr_;
		_sv sm4, sm3, sm2;
	};

	Banded3FrameSwipeMatrix(size_t band, size_t cols) :
		band_(band)
	{
		hgap_.resize(band + 3);
		score_.resize(band + 1);
		std::fill(hgap_.begin(), hgap_.end(), _sv());
		std::fill(score_.begin(), score_.end(), _sv());
	}

	inline ColumnIterator begin(size_t offset, size_t col)
	{
		return ColumnIterator(&hgap_[offset], &score_[offset]);
	}

	size_t band() const
	{
		return band_;
	}

private:
	const size_t band_;
	static thread_local MemBuffer<_sv> hgap_, score_;

};

template<typename _sv>
struct Banded3FrameSwipeTracebackMatrix
{

	typedef typename ScoreTraits<_sv>::Score Score;

	struct ColumnIterator
	{
		ColumnIterator(_sv* hgap_front, _sv* score_front, _sv* score_front1) :
			hgap_ptr_(hgap_front),
			score_ptr_(score_front),
			score_ptr1_(score_front1)
		{
			sm4 = ScoreTraits<_sv>::zero();
			sm3 = *(score_ptr_++);
			sm2 = *(score_ptr_);
		}
		inline void operator++()
		{
			++hgap_ptr_; ++score_ptr_; ++score_ptr1_;
			sm4 = sm3;
			sm3 = sm2;
			sm2 = *score_ptr_;
		}
		inline _sv hgap() const
		{
			return *(hgap_ptr_ + 3);
		}
		inline void set_hgap(const _sv& x)
		{
			*hgap_ptr_ = x;
		}
		inline void set_score(const _sv& x)
		{
			*score_ptr1_ = x;
		}
		void set_zero()
		{
			*(score_ptr1_ - 1) = ScoreTraits<_sv>::zero();
			*(score_ptr1_ - 2) = ScoreTraits<_sv>::zero();
			*(score_ptr1_ - 3) = ScoreTraits<_sv>::zero();
		}
		_sv *hgap_ptr_, *score_ptr_, *score_ptr1_;
		_sv sm4, sm3, sm2;
	};

	struct TracebackIterator
	{
		TracebackIterator(const Score *score, size_t band, int frame, int i, int j) :
			band_(band),
			score_(score),
			frame(frame),
			i(i),
			j(j)
		{
			assert(i >= 0 && j >= 0);
		}
		Score score() const
		{
			return *score_;
		}
		Score sm3() const
		{
			return *(score_ - (band_ + 1) * ScoreTraits<_sv>::CHANNELS);
		}
		Score sm4() const
		{
			return *(score_ - (band_ + 2) * ScoreTraits<_sv>::CHANNELS);
		}
		Score sm2() const
		{
			return *(score_ - band_ * ScoreTraits<_sv>::CHANNELS);
		}
		void walk_diagonal()
		{
			score_ -= (band_ + 1) * ScoreTraits<_sv>::CHANNELS;
			--i;
			--j;
			assert(i >= -1 && j >= -1);
		}
		void walk_forward_shift()
		{
			score_ -= (band_ + 2) * ScoreTraits<_sv>::CHANNELS;
			--i;
			--j;
			--frame;
			if (frame == -1) {
				frame = 2;
				--i;
			}
			assert(i >= -1 && j >= -1);
		}
		void walk_reverse_shift()
		{
			score_ -= band_ * ScoreTraits<_sv>::CHANNELS;
			--i;
			--j;
			++frame;
			if (frame == 3) {
				frame = 0;
				++i;
			}
			assert(i >= -1 && j >= -1);
		}
		pair<Edit_operation, int> walk_gap(int d0, int d1)
		{
			const int i0 = std::max(d0 + j, 0), j0 = std::max(i - d1, -1);
			const Score *h = score_ - (band_ - 2) * ScoreTraits<_sv>::CHANNELS, *h0 = score_ - (j - j0) * (band_ - 2) * ScoreTraits<_sv>::CHANNELS;
			const Score *v = score_ - 3 * ScoreTraits<_sv>::CHANNELS, *v0 = score_ - (i - i0 + 1) * 3 * ScoreTraits<_sv>::CHANNELS;
			const Score score = this->score();
			const Score e = score_matrix.gap_extend();
			Score g = score_matrix.gap_open() + e;
			int l = 1;
			while (v > v0 && h > h0) {
				if (score + g == *h) {
					walk_hgap(h, l);
					return std::make_pair(op_deletion, l);
				}
				else if (score + g == *v) {
					walk_vgap(v, l);
					return std::make_pair(op_insertion, l);
				}
				h -= (band_ - 2) * ScoreTraits<_sv>::CHANNELS;
				v -= 3 * ScoreTraits<_sv>::CHANNELS;
				++l;
				g += e;
			}
			while (v > v0) {
				if (score + g == *v) {
					walk_vgap(v, l);
					return std::make_pair(op_insertion, l);
				}
				v -= 3 * ScoreTraits<_sv>::CHANNELS;
				++l;
				g += e;
			}
			while (h > h0) {
				if (score + g == *h) {
					walk_hgap(h, l);
					return std::make_pair(op_deletion, l);
				}
				h -= (band_ - 2) * ScoreTraits<_sv>::CHANNELS;
				++l;
				g += e;
			}
			throw std::runtime_error("Traceback error.");
		}
		void walk_hgap(const Score *h, int l)
		{
			score_ = h;
			j -= l;
			assert(i >= -1 && j >= -1);
		}
		void walk_vgap(const Score *v, int l)
		{
			score_ = v;
			i -= l;
			assert(i >= -1 && j >= -1);
		}
		const size_t band_;
		const Score *score_;
		int frame, i, j;
	};

	TracebackIterator traceback(size_t col, int i0, int j, int dna_len, size_t channel, Score score) const
	{
		const int i_ = std::max(-i0, 0) * 3,
			i1 = (int)std::min(band_, size_t(dna_len - 2 - i0 * 3));
		const Score *s = (Score*)(&score_[col*(band_ + 1) + i_]) + channel;
		for (int i = i_; i < i1; ++i, s += ScoreTraits<_sv>::CHANNELS)
			if (*s == score)
				return TracebackIterator(s, band_, i % 3, i0 + i / 3, j);
		throw std::runtime_error("Trackback error.");
	}

	Banded3FrameSwipeTracebackMatrix(size_t band, size_t cols) :
		band_(band)
	{
		hgap_.resize(band + 3);
		score_.resize((band + 1) * (cols + 1));
		const _sv z = _sv();
		std::fill(hgap_.begin(), hgap_.end(), z);
		std::fill(score_.begin(), score_.begin() + band + 1, z);
		for (size_t i = 0; i < cols; ++i)
			score_[i*(band + 1) + band] = z;
	}

	inline ColumnIterator begin(size_t offset, size_t col)
	{
		return ColumnIterator(&hgap_[offset], &score_[col*(band_ + 1) + offset], &score_[(col + 1)*(band_ + 1) + offset]);
	}

	size_t band() const
	{
		return band_;
	}

private:
	const size_t band_;
	static thread_local MemBuffer<_sv> hgap_, score_;

};

template<typename _sv> thread_local MemBuffer<_sv> Banded3FrameSwipeMatrix<_sv>::hgap_;
template<typename _sv> thread_local MemBuffer<_sv> Banded3FrameSwipeMatrix<_sv>::score_;
template<typename _sv> thread_local MemBuffer<_sv> Banded3FrameSwipeTracebackMatrix<_sv>::hgap_;
template<typename _sv> thread_local MemBuffer<_sv> Banded3FrameSwipeTracebackMatrix<_sv>::score_;

struct Traceback {};
struct ScoreOnly {};

template<typename _sv, typename _traceback>
struct Banded3FrameSwipeMatrixRef
{
};

template<typename _sv>
struct Banded3FrameSwipeMatrixRef<_sv, Traceback>
{
	typedef Banded3FrameSwipeTracebackMatrix<_sv> type;
};

template<typename _sv>
struct Banded3FrameSwipeMatrixRef<_sv, ScoreOnly>
{
	typedef Banded3FrameSwipeMatrix<_sv> type;
};

template<typename _sv>
void traceback(sequence *query, Strand strand, int dna_len, const Banded3FrameSwipeTracebackMatrix<_sv> &dp, DpTarget &target, typename ScoreTraits<_sv>::Score max_score, int max_col, int channel, int i0, int i1, bool parallel)
{
	typedef typename ScoreTraits<_sv>::Score Score;
	const int j0 = i1 - (target.d_end - 1), d1 = target.d_end, d0 = target.d_begin;
	typename Banded3FrameSwipeTracebackMatrix<_sv>::TracebackIterator it(dp.traceback(max_col + 1, i0 + max_col, j0 + max_col, dna_len, channel, max_score));
	if (parallel)
		target.tmp = new Hsp();
	else
		target.out->emplace_back();
		
	Hsp &out = parallel ? *target.tmp : target.out->back();
	out.score = target.score = ScoreTraits<_sv>::int_score(max_score);
	out.transcript.reserve(size_t(out.score * config.transcript_len_estimate));

	out.set_end(it.i + 1, it.j + 1, Frame(strand, it.frame), dna_len);

	while (it.score() > ScoreTraits<_sv>::zero_score()) {
		const Letter q = query[it.frame][it.i], s = target.seq[it.j];
		const Score m = score_matrix(q, s), score = it.score();
		if (score == it.sm3() + m) {
			out.push_match(q, s, m > (Score)0);
			it.walk_diagonal();
		}
		else if (score == it.sm4() + m - score_matrix.frame_shift()) {
			out.push_match(q, s, m > (Score)0);
			out.transcript.push_back(op_frameshift_forward);
			it.walk_forward_shift();
		}
		else if (score == it.sm2() + m - score_matrix.frame_shift()) {
			out.push_match(q, s, m > (Score)0);
			out.transcript.push_back(op_frameshift_reverse);
			it.walk_reverse_shift();
		}
		else {
			const pair<Edit_operation, int> g(it.walk_gap(d0, d1));
			out.push_gap(g.first, g.second, &target.seq[it.j + g.second]);
		}
	}

	out.set_begin(it.i + 1, it.j + 1, Frame(strand, it.frame), dna_len);
	out.transcript.reverse();
	out.transcript.push_terminator();
}

template<typename _sv>
void traceback(sequence *query, Strand strand, int dna_len, const Banded3FrameSwipeMatrix<_sv> &dp, DpTarget &target, typename ScoreTraits<_sv>::Score max_score, int max_col, int channel, int i0, int i1, bool parallel)
{
	if (parallel)
		target.tmp = new Hsp();
	else
		target.out->emplace_back();
	Hsp &out = parallel ? *target.tmp : target.out->back();
	
	const int j0 = i1 - (target.d_end - 1);
	out.score = target.score = ScoreTraits<_sv>::int_score(max_score);
	out.query_range.end_ = std::min(i0 + max_col + (int)dp.band() / 3 / 2, (int)query[0].length());
	out.query_range.begin_ = std::max(out.query_range.end_ - (j0 + max_col), 0);
	out.frame = strand == FORWARD ? 0 : 3;
	out.query_source_range = TranslatedPosition::absolute_interval(TranslatedPosition(out.query_range.begin_, Frame(out.frame)), TranslatedPosition(out.query_range.end_, Frame(out.frame)), dna_len);
}

template<typename _sv, typename _traceback>
void banded_3frame_swipe(const TranslatedSequence &query, Strand strand, vector<DpTarget>::iterator subject_begin, vector<DpTarget>::iterator subject_end, DpStat &stat, bool parallel)
{
	typedef typename Banded3FrameSwipeMatrixRef<_sv, _traceback>::type Matrix;
	typedef typename ScoreTraits<_sv>::Score Score;

	assert(subject_end - subject_begin <= ScoreTraits<_sv>::CHANNELS);
	sequence q[3];
	query.get_strand(strand, q);
	const int qlen = (int)q[0].length(), qlen2 = (int)q[1].length(), qlen3 = (int)q[2].length();

	int band = 0;
	for (vector<DpTarget>::const_iterator j = subject_begin; j < subject_end; ++j)
		band = std::max(band, j->d_end - j->d_begin);

	int i0 = INT_MAX, i1 = INT_MAX;
	for (vector<DpTarget>::iterator j = subject_begin; j < subject_end; ++j) {
		/*if (j->d_end - j->d_begin < band) {
			const int top_max = j->d_begin - (-(int)(j->seq.length() - 1)), bottom_max = qlen - j->d_end;
			int diff = band - (j->d_end - j->d_begin);
			int d = std::min(std::max(diff / 2, diff - bottom_max), top_max);
			j->d_begin -= d;
			diff -= d;
			if (diff > bottom_max)
				throw std::runtime_error("");
			j->d_end += std::min(diff, bottom_max);
		}*/
		j->d_begin = j->d_end - band;
		int i2 = std::max(j->d_end - 1, 0);
		i1 = std::min(i1, i2);
		i0 = std::min(i0, i2 + 1 - band);
	}

	TargetIterator<ScoreTraits<_sv>::CHANNELS> targets(subject_begin, subject_end, i1, qlen);
	Matrix dp(band * 3, targets.cols);

	const _sv open_penalty(score_matrix.gap_open() + score_matrix.gap_extend()),
		extend_penalty(score_matrix.gap_extend()),
		frameshift_penalty(score_matrix.frame_shift());
	
	SwipeProfile<_sv> profile;
	Score best[ScoreTraits<_sv>::CHANNELS];
	int max_col[ScoreTraits<_sv>::CHANNELS];
	for (int i = 0; i < ScoreTraits<_sv>::CHANNELS; ++i)
		best[i] = ScoreTraits<_sv>::zero_score();

	int j = 0;
	while (targets.active.size() > 0) {
		const int i0_ = std::max(i0, 0), i1_ = std::min(i1, qlen - 1);
		if (i0_ > i1_)
			break;
		typename Matrix::ColumnIterator it(dp.begin((i0_ - i0) * 3, j));
		if (i0_ - i0 > 0)
			it.set_zero();
		_sv vgap0, vgap1, vgap2, hgap, col_best;
		vgap0 = vgap1 = vgap2 = col_best = ScoreTraits<_sv>::zero();

		profile.set(targets.get());
		for (int i = i0_; i <= i1_; ++i) {
			hgap = it.hgap();
			_sv next = cell_update<_sv>(it.sm3, it.sm4, it.sm2, profile.get(q[0][i]), extend_penalty, open_penalty, frameshift_penalty, hgap, vgap0, col_best);
			it.set_hgap(hgap);
			it.set_score(next);
			++it;

			if (i >= qlen2)
				break;
			hgap = it.hgap();
			next = cell_update<_sv>(it.sm3, it.sm4, it.sm2, profile.get(q[1][i]), extend_penalty, open_penalty, frameshift_penalty, hgap, vgap1, col_best);
			it.set_hgap(hgap);
			it.set_score(next);
			++it;

			if (i >= qlen3)
				break;
			hgap = it.hgap();
			next = cell_update<_sv>(it.sm3, it.sm4, it.sm2, profile.get(q[2][i]), extend_penalty, open_penalty, frameshift_penalty, hgap, vgap2, col_best);
			it.set_hgap(hgap);
			it.set_score(next);
			++it;
		}

#ifdef DP_STAT
		stat.net_cells += targets.live * (i1_ - i0_ + 1) * 3;
		stat.gross_cells += ScoreTraits<_sv>::CHANNELS * (i1_ - i0_ + 1) * 3;
#endif

		Score col_best_[ScoreTraits<_sv>::CHANNELS];
		store_sv(col_best, col_best_);
		for (int i = 0; i < targets.active.size();) {
			int channel = targets.active[i];
			if (!targets.inc(channel))
				targets.active.erase(i);
			else
				++i;
			if (col_best_[channel] > best[channel]) {
				best[channel] = col_best_[channel];
				max_col[channel] = j;
			}
		}
		++i0;
		++i1;
		++j;
	}
	
	for (int i = 0; i < targets.n_targets; ++i) {
		if (best[i] < ScoreTraits<_sv>::max_score()) {
			subject_begin[i].overflow = false;
			traceback<_sv>(q, strand, (int)query.source().length(), dp, subject_begin[i], best[i], max_col[i], i, i0 - j, i1 - j, parallel);
		}
		else
			subject_begin[i].overflow = true;
	}
}

template<typename _sv>
void banded_3frame_swipe_targets(vector<DpTarget>::iterator begin,
	vector<DpTarget>::iterator end,
	bool score_only,
	const TranslatedSequence &query,
	Strand strand,
	DpStat &stat,
	bool parallel,
	bool overflow_only)
{
	for (vector<DpTarget>::iterator i = begin; i < end; i += ScoreTraits<_sv>::CHANNELS) {
		if (!overflow_only || i->overflow) {
			if (score_only || config.disable_traceback)
				banded_3frame_swipe<_sv, ScoreOnly>(query, strand, i, i + std::min(vector<DpTarget>::iterator::difference_type(ScoreTraits<_sv>::CHANNELS), end - i), stat, parallel);
			else
				banded_3frame_swipe<_sv, Traceback>(query, strand, i, i + std::min(vector<DpTarget>::iterator::difference_type(ScoreTraits<_sv>::CHANNELS), end - i), stat, parallel);
		}
	}
}

void banded_3frame_swipe_worker(vector<DpTarget>::iterator begin,
	vector<DpTarget>::iterator end,
	Atomic<size_t> *next,
	bool score_only,
	const TranslatedSequence *query,
	Strand strand)
{
	DpStat stat;
	size_t pos;
	while (begin + (pos = next->post_add(config.swipe_chunk_size)) < end)
#ifdef __SSE2__
		banded_3frame_swipe_targets<score_vector<int16_t>>(begin + pos, min(begin + pos + config.swipe_chunk_size, end), score_only, *query, strand, stat, true, false);
#else
		banded_3frame_swipe_targets<int32_t>(begin + pos, min(begin + pos + config.swipe_chunk_size, end), score_only, *query, strand, stat, true, false);
#endif
}

void banded_3frame_swipe(const TranslatedSequence &query, Strand strand, vector<DpTarget>::iterator target_begin, vector<DpTarget>::iterator target_end, DpStat &stat, bool score_only, bool parallel)
{
#ifdef __SSE2__
	task_timer timer("Banded 3frame swipe (sort)", parallel ? 3 : UINT_MAX);
	std::stable_sort(target_begin, target_end);
	if (parallel) {
		timer.go("Banded 3frame swipe (run)");
		vector<thread> threads;
		Atomic<size_t> next(0);
		for (size_t i = 0; i < config.threads_; ++i)
			threads.emplace_back(banded_3frame_swipe_worker,
				target_begin,
				target_end,
				&next,
				score_only,
				&query,
				strand);
		for (auto &t : threads)
			t.join();
		timer.go("Banded 3frame swipe (merge)");
		for (auto i = target_begin; i < target_end; ++i) {
			i->out->push_back(*i->tmp);
			delete i->tmp;
		}
	}
	else
		banded_3frame_swipe_targets<score_vector<int16_t>>(target_begin, target_end, score_only, query, strand, stat, false, false);

	banded_3frame_swipe_targets<int32_t>(target_begin, target_end, score_only, query, strand, stat, false, true);
#else
	banded_3frame_swipe_targets<int32_t>(target_begin, target_end, score_only, query, strand, stat, false, false);
#endif
}