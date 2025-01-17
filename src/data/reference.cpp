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

#include <limits>
#include <iostream>
#include <set>
#include <map>
#include <memory>
#include "../basic/config.h"
#include "reference.h"
#include "load_seqs.h"
#include "../util/seq_file_format.h"
#include "../util/log_stream.h"
#include "../basic/masking.h"
#include "../util/io/text_input_file.h"
#include "taxonomy.h"
#include "../util/io/file_backed_buffer.h"
#include "taxon_list.h"
#include "taxonomy_nodes.h"
#include "../util/algo/MurmurHash3.h"
#include "../util/io/record_reader.h"

String_set<0>* ref_ids::data_ = 0;
Partitioned_histogram ref_hst;
unsigned current_ref_block;
Sequence_set* ref_seqs::data_ = 0;
bool blocked_processing;

using namespace std;

Serializer& operator<<(Serializer &s, const ReferenceHeader2 &h)
{
	s.unset(Serializer::VARINT);
	s << sizeof(ReferenceHeader2);
	s.write(h.hash, sizeof(h.hash));
	s << h.taxon_array_offset << h.taxon_array_size << h.taxon_nodes_offset << h.taxon_names_offset;
	return s;
}

Deserializer& operator>>(Deserializer &d, ReferenceHeader2 &h)
{
	d.read_record().read(h.hash, sizeof(h.hash))
		>> h.taxon_array_offset
		>> h.taxon_array_size
		>> h.taxon_nodes_offset
		>> h.taxon_names_offset
		>> Finish();
	return d;
}


struct Pos_record
{
	Pos_record()
	{}
	Pos_record(uint64_t pos, size_t len):
		pos(pos),
		seq_len(uint32_t(len))
	{}
	uint64_t pos;
	uint32_t seq_len;
};

void DatabaseFile::init()
{
	read_header(*this, ref_header);
	if (ref_header.build < min_build_required || ref_header.db_version < MIN_DB_VERSION)
		throw std::runtime_error("Database was built with an older version of Diamond and is incompatible.");
	if (ref_header.db_version > ReferenceHeader::current_db_version)
		throw std::runtime_error("Database was built with a newer version of Diamond and is incompatible.");
	if (ref_header.sequences == 0)
		throw std::runtime_error("Incomplete database file. Database building did not complete successfully.");
	*this >> header2;
	pos_array_offset = ref_header.pos_array_offset;
}

DatabaseFile::DatabaseFile(const string &input_file):
	InputFile(input_file, InputFile::BUFFERED),
	temporary(false)
{
	init();
}

DatabaseFile::DatabaseFile(TempFile &tmp_file):
	InputFile(tmp_file, 0),
	temporary(true)
{
	init();
}

void DatabaseFile::close() {
	if (temporary)
		InputFile::close_and_delete();
	else
		InputFile::close();
}

void DatabaseFile::read_header(InputFile &stream, ReferenceHeader &header)
{
	if (stream.read(&header, 1) != 1)
		throw Database_format_exception();
	if (header.magic_number != ReferenceHeader().magic_number)
		throw Database_format_exception();
}

bool DatabaseFile::has_taxon_id_lists()
{
	return header2.taxon_array_offset != 0;
}

bool DatabaseFile::has_taxon_nodes()
{
	return header2.taxon_nodes_offset != 0;
}

bool DatabaseFile::has_taxon_scientific_names() {
	return header2.taxon_names_offset != 0;

}

void DatabaseFile::rewind()
{
	pos_array_offset = ref_header.pos_array_offset;
}

void push_seq(const sequence &seq, const sequence &id, uint64_t &offset, vector<Pos_record> &pos_array, OutputFile &out, size_t &letters, size_t &n_seqs)
{	
	pos_array.emplace_back(offset, seq.length());
	out.write("\xff", 1);
	out.write(seq.data(), seq.length());
	out.write("\xff", 1);
	out.write(id.data(), id.length() + 1);
	letters += seq.length();
	++n_seqs;
	offset += seq.length() + id.length() + 3;
}

void make_db(TempFile **tmp_out)
{
	message_stream << "Database file: " << config.input_ref_file << endl;
	
	Timer total;
	total.start();
	if (config.input_ref_file == "")
		std::cerr << "Input file parameter (--in) is missing. Input will be read from stdin." << endl;
	task_timer timer("Opening the database file", true);
	unique_ptr<TextInputFile> db_file (new TextInputFile(config.input_ref_file));
	
	OutputFile *out = tmp_out ? new TempFile() : new OutputFile(config.database);
	ReferenceHeader header;
	ReferenceHeader2 header2;

	out->write(&header, 1);
	*out << header2;

	size_t letters = 0, n = 0, n_seqs = 0;
	uint64_t offset = out->tell();

	Sequence_set *seqs;
	String_set<0> *ids;
	const FASTA_format format;
	vector<Pos_record> pos_array;
	FileBackedBuffer accessions;

	try {
		while ((timer.go("Loading sequences"), n = load_seqs(*db_file, format, &seqs, ids, 0, nullptr, (size_t)(1e9), string())) > 0) {
			if (config.masking == 1) {
				timer.go("Masking sequences");
				mask_seqs(*seqs, Masking::get(), false);
			}
			timer.go("Writing sequences");
			for (size_t i = 0; i < n; ++i) {
				sequence seq = (*seqs)[i];
				if (seq.length() == 0)
					throw std::runtime_error("File format error: sequence of length 0 at line " + to_string(db_file->line_count));
				push_seq(seq, (*ids)[i], offset, pos_array, *out, letters, n_seqs);
			}
			if (!config.prot_accession2taxid.empty()) {
				timer.go("Writing accessions");
				for (size_t i = 0; i < n; ++i)
					accessions << Taxonomy::Accession::from_title((*ids)[i].c_str());
			}
			timer.go("Hashing sequences");
			for (size_t i = 0; i < n; ++i) {
				sequence seq = (*seqs)[i], id = (*ids)[i];
				MurmurHash3_x64_128(seq.data(), (int)seq.length(), header2.hash, header2.hash);
				MurmurHash3_x64_128(id.data(), (int)id.length(), header2.hash, header2.hash);
			}
			delete seqs;
			delete ids;
		}
	}
	catch (std::exception&) {
		out->close();
		out->remove();
		throw;
	}

	timer.finish();
	
	timer.go("Writing trailer");
	header.pos_array_offset = offset;
	pos_array.emplace_back(offset, 0);
	out->write_raw(pos_array);
	timer.finish();

	taxonomy.init();
	if (!config.prot_accession2taxid.empty()) {
		header2.taxon_array_offset = out->tell();
		TaxonList::build(*out, accessions.rewind(), n_seqs);
		header2.taxon_array_size = out->tell() - header2.taxon_array_offset;
	}
	if (!config.nodesdmp.empty()) {
		header2.taxon_nodes_offset = out->tell();
		TaxonomyNodes::build(*out);
	}
	if (!config.namesdmp.empty()) {
		header2.taxon_names_offset = out->tell();
		*out << taxonomy.name_;
	}

	timer.go("Closing the input file");
	db_file->close();
	
	timer.go("Closing the database file");
	header.letters = letters;
	header.sequences = n_seqs;
	out->seek(0);
	out->write(&header, 1);
	*out << header2;
	if (tmp_out) {
		*tmp_out = static_cast<TempFile*>(out);
	} else {
		out->close();
		delete out;
	}

	timer.finish();
	message_stream << "Database hash = " << hex_print(header2.hash, 16) << endl;
	message_stream << "Processed " << n_seqs << " sequences, " << letters << " letters." << endl;
	message_stream << "Total time = " << total.getElapsedTimeInSec() << "s" << endl;
}

void DatabaseFile::seek_seq(size_t i) {
	pos_array_offset = ref_header.pos_array_offset + sizeof(Pos_record)*i;
}

size_t DatabaseFile::tell_seq() const {
	return (pos_array_offset - ref_header.pos_array_offset) / sizeof(Pos_record);
}

void DatabaseFile::seek_direct() {
	seek(sizeof(ReferenceHeader) + sizeof(ReferenceHeader2) + 8);
}

bool DatabaseFile::load_seqs(vector<unsigned> &block_to_database_id, size_t max_letters, Sequence_set **dst_seq, String_set<0> **dst_id, bool load_ids, const vector<bool> *filter)
{
	task_timer timer("Loading reference sequences");
	seek(pos_array_offset);
	size_t database_id = tell_seq();
	size_t letters = 0, seqs = 0, id_letters = 0, seqs_processed = 0;
	vector<uint64_t> filtered_pos;
	block_to_database_id.clear();

	*dst_seq = new Sequence_set;
	if(load_ids) *dst_id = new String_set<0>;

	Pos_record r;
	read(&r, 1);
	uint64_t start_offset = r.pos;
	bool last = false;

	while (r.seq_len > 0 && letters < max_letters) {
		Pos_record r_next;
		read(&r_next, 1);
		if (!filter || (*filter)[database_id]) {
			letters += r.seq_len;
			(*dst_seq)->reserve(r.seq_len);
			const size_t id_len = r_next.pos - r.pos - r.seq_len - 3;
			id_letters += id_len;
			if (load_ids) (*dst_id)->reserve(id_len);
			++seqs;
			block_to_database_id.push_back((unsigned)database_id);
			if (filter) filtered_pos.push_back(last ? 0 : r.pos);
			last = true;
		}
		else
			last = false;
		pos_array_offset += sizeof(Pos_record);
		++database_id;
		++seqs_processed;
		r = r_next;
	}

	if (seqs == 0) {
		delete (*dst_seq);
		(*dst_seq) = NULL;
		if (load_ids) delete (*dst_id);
		(*dst_id) = NULL;
		return false;
	}

	(*dst_seq)->finish_reserve();
	if(load_ids) (*dst_id)->finish_reserve();
	seek(start_offset);

	for (size_t n = 0; n < seqs; ++n) {
		if (filter && filtered_pos[n]) seek(filtered_pos[n]);
		read((*dst_seq)->ptr(n) - 1, (*dst_seq)->length(n) + 2);
		*((*dst_seq)->ptr(n) - 1) = sequence::DELIMITER;
		*((*dst_seq)->ptr(n) + (*dst_seq)->length(n)) = sequence::DELIMITER;
		if (load_ids)
			read((*dst_id)->ptr(n), (*dst_id)->length(n) + 1);
		else
			if (!seek_forward('\0')) throw std::runtime_error("Unexpected end of file.");
		Masking::get().remove_bit_mask((*dst_seq)->ptr(n), (*dst_seq)->length(n));
		if (!config.sfilt.empty() && strstr((**dst_id)[n].c_str(), config.sfilt.c_str()) == 0)
			memset((*dst_seq)->ptr(n), value_traits.mask_char, (*dst_seq)->length(n));
	}
	timer.finish();
	(*dst_seq)->print_stats();

	blocked_processing = seqs_processed < ref_header.sequences;
	return true;
}

void DatabaseFile::read_seq(string &id, vector<char> &seq)
{
	char c;
	read(&c, 1);
	read_until(seq, '\xff');
	read_until(id, '\0');
}

void DatabaseFile::get_seq()
{
	std::map<string, string> seq_titles;
	if (!config.query_file.empty()) {
		TextInputFile list(config.query_file);
		while (list.getline(), !list.eof()) {
			const vector<string> t(tokenize(list.line.c_str(), "\t"));
			if (t.size() != 2)
				throw std::runtime_error("Query file format error.");
			seq_titles[t[0]] = t[1];
		}
		list.close();
	}

	vector<Letter> seq;
	string id;
	bool all = config.seq_no.size() == 0 && seq_titles.empty();
	std::set<size_t> seqs;
	if (!all)
		for (vector<string>::const_iterator i = config.seq_no.begin(); i != config.seq_no.end(); ++i)
			seqs.insert(atoi(i->c_str()) - 1);
	const size_t max_letters = config.chunk_size == 0.0 ? std::numeric_limits<size_t>::max() : (size_t)(config.chunk_size*1e9);
	size_t letters = 0;
	TextBuffer buf;
	OutputFile out(config.output_file);
	for (size_t n = 0; n < ref_header.sequences; ++n) {
		read_seq(id, seq);
		std::map<string, string>::const_iterator mapped_title = seq_titles.find(blast_id(id));
		if (all || seqs.find(n) != seqs.end() || mapped_title != seq_titles.end()) {
			buf << '>' << (mapped_title != seq_titles.end() ? mapped_title->second : id) << '\n';
			if (config.reverse) {
				sequence(seq).print(buf, value_traits, sequence::Reversed());
				buf << '\n';
			}
			else if (config.hardmasked) {
				sequence(seq).print(buf, value_traits, sequence::Hardmasked());
				buf << '\n';
			}
			else
				buf << sequence(seq) << '\n';
		}
		out.write(buf.get_begin(), buf.size());
		letters += seq.size();
		if (letters >= max_letters)
			break;
		seq.clear();
		id.clear();
		buf.clear();
	}

	out.close();
}

void db_info()
{
	InputFile db_file(config.database);
	ReferenceHeader header;
	DatabaseFile::read_header(db_file, header);
	cout << "Database format version = " << header.db_version << endl;
	cout << "Diamond build = " << header.build << endl;
	cout << "Sequences = " << header.sequences << endl;
	cout << "Letters = " << header.letters << endl;
	db_file.close();
}

bool DatabaseFile::is_diamond_db(const string &file_name) {
	if (file_name == "-")
		return false;
	InputFile db_file(file_name);
	uint64_t magic_number;
	bool r = db_file.read(&magic_number, 1) == 1 && magic_number == ReferenceHeader::MAGIC_NUMBER;
	db_file.close();
	return r;
}

DatabaseFile* DatabaseFile::auto_create_from_fasta() {
	if (!is_diamond_db(config.database)) {
		message_stream << "Database file is not a DIAMOND database, treating as FASTA." << endl;
		config.input_ref_file = config.database;
		TempFile *db;
		make_db(&db);
		DatabaseFile *r(new DatabaseFile(*db));
		delete db;
		return r;
	}
	else
		return new DatabaseFile(config.database);
}