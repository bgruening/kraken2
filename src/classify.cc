/*
 * Copyright 2013-2023, Derrick Wood <dwood@cs.jhu.edu>
 *
 * This file is part of the Kraken 2 taxonomic sequence classification system.
 */

#include <sys/wait.h>
#include <sys/types.h>

#include "kraken2_headers.h"
#include "kv_store.h"
#include "taxonomy.h"
#include "seqreader.h"
#include "mmscanner.h"
#include "compact_hash.h"
#include "kraken2_data.h"
#include "aa_translate.h"
#include "reports.h"
#include "utilities.h"
using namespace kraken2;

using std::cerr;
using std::endl;
using std::ifstream;
using std::map;
using std::ostringstream;
using std::ofstream;
using std::string;
using std::vector;
using namespace kraken2;

static const size_t NUM_FRAGMENTS_PER_THREAD = 10000;
static const taxid_t MATE_PAIR_BORDER_TAXON = TAXID_MAX;
static const taxid_t READING_FRAME_BORDER_TAXON = TAXID_MAX - 1;
static const taxid_t AMBIGUOUS_SPAN_TAXON = TAXID_MAX - 2;

using IndexData = std::tuple<IndexOptions, Taxonomy, CompactHashTable *>;

struct Options {
  string index_filename;
  string taxonomy_filename;
  string options_filename;
  string report_filename;
  string classified_output_filename;
  string unclassified_output_filename;
  string kraken_output_filename;
  bool mpa_style_report;
  bool report_kmer_data;
  bool quick_mode;
  bool report_zero_counts;
  bool use_translated_search;
  bool print_scientific_name;
  double confidence_threshold;
  int num_threads;
  bool paired_end_processing;
  bool single_file_pairs;
  int minimum_quality_score;
  int minimum_hit_groups;
  bool use_memory_mapping;
  bool match_input_order;
  std::vector<char *> filenames;
  bool daemon_mode;

  void reset() {
    quick_mode = false;
    confidence_threshold = 0;
    paired_end_processing = false;
    single_file_pairs = false;
    num_threads = 1;
    mpa_style_report = false;
    report_kmer_data = false;
    report_zero_counts = false;
    use_translated_search = false;
    print_scientific_name = false;
    minimum_quality_score = 0;
    minimum_hit_groups = 0;
    use_memory_mapping = false;
    daemon_mode = false;

    index_filename.clear();
    taxonomy_filename.clear();
    options_filename.clear();
    report_filename.clear();
    classified_output_filename.clear();
    unclassified_output_filename.clear();
    kraken_output_filename.clear();
    filenames.clear();
  }
};

struct ClassificationStats {
  uint64_t total_sequences;
  uint64_t total_bases;
  uint64_t total_classified;
};

struct OutputStreamData {
  bool initialized;
  bool printing_sequences;
  std::ostream *classified_output1;
  std::ostream *classified_output2;
  std::ostream *unclassified_output1;
  std::ostream *unclassified_output2;
  std::ostream *kraken_output;
};

struct OutputData {
public:
  uint64_t block_id;
  string kraken_str;
  string classified_out1_str;
  string classified_out2_str;
  string unclassified_out1_str;
  string unclassified_out2_str;
};

void ParseCommandLine(int argc, char **argv, Options &opts);
void usage(int exit_code=EX_USAGE);
void ProcessFiles(const char *filename1, const char *filename2,
    KeyValueStore *hash, Taxonomy &tax,
    IndexOptions &idx_opts, Options &opts, ClassificationStats &stats,
    OutputStreamData &outputs, taxon_counters_t &total_taxon_counters);
taxid_t ClassifySequence(Sequence &dna, Sequence &dna2, ostringstream &koss,
    KeyValueStore *hash, Taxonomy &tax, IndexOptions &idx_opts,
    Options &opts, ClassificationStats &stats, MinimizerScanner &scanner,
    vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
    vector<string> &tx_frames, taxon_counters_t &my_taxon_counts);
void AddHitlistString(ostringstream &oss, vector<taxid_t> &taxa,
    Taxonomy &taxonomy);
taxid_t ResolveTree(taxon_counts_t &hit_counts,
    Taxonomy &tax, size_t total_minimizers, Options &opts);
void ReportStats(struct timeval time1, struct timeval time2,
    ClassificationStats &stats);
void InitializeOutputs(Options &opts, OutputStreamData &outputs, SequenceFormat format);
void MaskLowQualityBases(Sequence &dna, int minimum_quality_score);


void RemoveBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int daemonize() {
  pid_t pid;

  if ((pid = fork()) < 0) {
    errx(1, "fork");
  }

  if (pid == 0) {
    if (setsid() == -1) {
      perror("setdsid");
      exit(1);
    }
  } else {
    exit(0);
  }

  if ((pid = fork()) < 0) {
    perror("fork");
    exit(1);
  }

  if (pid != 0) {
    exit(0);
  }

  mkfifo("/tmp/classify_stdin", S_IRWXU);
  mkfifo("/tmp/classify_stdout", S_IRWXU);

  int read_fd = open("/tmp/classify_stdin", O_RDONLY | O_NONBLOCK);
  // keep these open so that to daemon does not
  // receive EOF when the external process closes
  // its end of the fifo
  int dummy_fd_1 = open("/tmp/classify_stdin", O_WRONLY);
  (void)dummy_fd_1;
  int dummy_fd_2 = open("/tmp/classify_stdout", O_RDONLY | O_NONBLOCK);

  int write_fd = open("/tmp/classify_stdout", O_WRONLY);

  RemoveBlocking(read_fd);
  RemoveBlocking(dummy_fd_2);

  for (int fd = 0; fd < 2; fd++) {
    close(fd);
  }

  dup2(read_fd, 0);
  dup2(write_fd, 1);
  dup2(write_fd, 2);

  return (0);
}

void OpenFifos(Options &opts, pid_t pid) {
  char stdin_filename[256];
  char stdout_filename[256];

  snprintf(stdin_filename, 256, "/tmp/classify_%d_stdin", pid);
  snprintf(stdout_filename, 256, "/tmp/classify_%d_stdout", pid);

  mkfifo(stdin_filename, S_IRWXU);
  mkfifo(stdout_filename, S_IRWXU);

  int read_fd = -1;
  // if we are expecting input from stdin, open the
  // FIFO in blocking mode and wait for the input.
  if (opts.filenames.empty()) {
    read_fd = open(stdin_filename, O_RDONLY);
  } else {
    read_fd = open(stdin_filename, O_RDONLY | O_NONBLOCK);
    // keep these open so that to daemon does not
    // receive EOF when the external process closes
    // its end of the fifo
    int dummy_fd_1 = open(stdin_filename, O_WRONLY);
    (void)dummy_fd_1;
  }

  // If we are outputting to a file open the write
  // FIFO in non-blocking mode and keep a read end open
  // so that we do not block the process.
  int dummy_fd_2 = -1;
  if (!opts.kraken_output_filename.empty()) {
    dummy_fd_2 = open(stdout_filename, O_RDONLY | O_NONBLOCK);
  }
  int write_fd = open(stdout_filename, O_WRONLY);

  if (opts.kraken_output_filename.empty()) {
    RemoveBlocking(read_fd);
  } else {
    RemoveBlocking(dummy_fd_2);
  }

  for (int fd = 0; fd < 3; fd++) {
    close(fd);
  }

  dup2(read_fd, 0);
  dup2(write_fd, 1);
  dup2(write_fd, 2);
}

IndexData load_index(Options &opts) {
  cerr << "Loading database information...";

  IndexOptions idx_opts = {0};
  ifstream idx_opt_fs(opts.options_filename);
  struct stat sb;
  if (stat(opts.options_filename.c_str(), &sb) < 0)
    errx(EX_OSERR, "unable to get filesize of %s", opts.options_filename.c_str());
  auto opts_filesize = sb.st_size;
  idx_opt_fs.read((char *) &idx_opts, opts_filesize);
  opts.use_translated_search = ! idx_opts.dna_db;
  Taxonomy taxonomy(opts.taxonomy_filename, opts.use_memory_mapping);
  IndexData index_data {
    idx_opts, std::move(taxonomy),
    new CompactHashTable(opts.index_filename, opts.use_memory_mapping)
  };
  cerr << " done." << endl;

  return index_data;
}

void classify(Options &opts, IndexData& index_data) {
  taxon_counters_t taxon_counters; // stats per taxon
  IndexOptions idx_opts = std::get<0>(index_data);
  Taxonomy &taxonomy = std::get<1>(index_data);
  KeyValueStore *hash_ptr = std::get<2>(index_data);

  omp_set_num_threads(opts.num_threads);

  ClassificationStats stats = {0, 0, 0};

  OutputStreamData outputs = { false, false, nullptr, nullptr, nullptr, nullptr, &std::cout };
  // taxon_counters.reserve(taxonomy.node_count());
  struct timeval tv1, tv2;
  gettimeofday(&tv1, nullptr);
  if (opts.filenames.empty()) {
    if (opts.paired_end_processing && ! opts.single_file_pairs)
      errx(EX_USAGE, "paired end processing used with no files specified");
    ProcessFiles(nullptr, nullptr, hash_ptr, taxonomy, idx_opts, opts, stats, outputs, taxon_counters);
  }
  else {
    for (size_t i = 0; i < opts.filenames.size(); i++) {
      if (opts.paired_end_processing && ! opts.single_file_pairs) {
        if (i + 1 == opts.filenames.size()) {
          errx(EX_USAGE, "paired end processing used with unpaired file");
        }
        ProcessFiles(opts.filenames[i], opts.filenames[i+1], hash_ptr, taxonomy, idx_opts, opts, stats, outputs, taxon_counters);
        i += 1;
      } else {
        ProcessFiles(opts.filenames[i], nullptr, hash_ptr, taxonomy, idx_opts, opts, stats, outputs, taxon_counters);
      }
    }
  }
  gettimeofday(&tv2, nullptr);

  // delete hash_ptr;

  ReportStats(tv1, tv2, stats);
  if (! opts.report_filename.empty()) {
    if (opts.mpa_style_report)
      ReportMpaStyle(opts.report_filename, opts.report_zero_counts, taxonomy,
          taxon_counters);
    else {
      auto total_unclassified = stats.total_sequences - stats.total_classified;
      ReportKrakenStyle(opts.report_filename, opts.report_zero_counts,
          opts.report_kmer_data, taxonomy,
          taxon_counters, stats.total_sequences, total_unclassified);
    }
  }
}

void TokenizeString(std::vector<char *>& argv, const char *str) {
  argv.resize(0);
  const char *sep = " ";
  char *str_cpy = new char[strlen(str) + 1];
  char *token;

  strcpy(str_cpy, str);
  for (token = strtok(str_cpy, sep); token;
       token = strtok(NULL, sep)) {
    int token_len = strlen(token);
    char *word = NULL;
    word = new char[token_len + 1];
    strcpy(word, token);
    argv.push_back(word);
  }

  delete[] str_cpy;
}

void ClassifyDaemon(Options opts) {
  daemonize();

  std::vector<char *> args;
  std::map<std::string, IndexData> indexes;
  std::stringstream ss;
  char *cmdline = NULL;
  size_t linecap = 0;
  ssize_t line_len;
  bool stop = false;

  int pid_file = open("/tmp/classify.pid", O_CREAT | O_WRONLY | O_TRUNC, 0600);
  ss << getpid() << "\n";
  write(pid_file, ss.str().c_str(), ss.str().size());
  ss.str("");
  close(pid_file);

  auto index_data = load_index(opts);
  indexes.emplace(opts.index_filename, std::move(index_data));

  while (!stop) {
    pid_t pid;
    if ((pid = fork()) < 0) {
      perror("fork");
      exit(EXIT_FAILURE);
    }
    if (pid == 0) {
      OpenFifos(opts, getpid());
      classify(opts, indexes[opts.index_filename]);
      for (auto i = 0; i < args.size(); i++) {
        delete[] args[i];
      }
      exit(EXIT_SUCCESS);
    } else {
      std::cout << "PID: " << pid << std::endl;
      if (wait(NULL) != pid) {
        perror("wait");
        exit(EXIT_FAILURE);
      }
      std::cout << "DONE" << std::endl;
      ss << "/tmp/classify_" << pid << "_stdin";
      unlink(ss.str().c_str());
      ss.str("");
      ss << "/tmp/classify_" << pid << "_stdout";
      unlink(ss.str().c_str());
      ss.str("");
    }

    while (true) {
      line_len = getline(&cmdline, &linecap, stdin);
      if (line_len < 2)
        continue;
      if (cmdline == std::string("PING\n")) {
        std::cerr << "OK" << std::endl;
        continue;
      } else if (cmdline == std::string("STOP\n")) {
        std::cerr << "OK" << std::endl;
        stop = true;
      }
      cmdline[line_len - 1] = ' ';
      break;
    }

    TokenizeString(args, cmdline);
    opts.reset();
    ParseCommandLine(args.size(), &args[0], opts);
    if (indexes.find(opts.index_filename) == indexes.end()) {
      auto index_data = load_index(opts);
      indexes.emplace(opts.index_filename, std::move(index_data));
    }
  }

  for (auto &entry : indexes) {
    auto &index_data = entry.second;
    delete std::get<2>(index_data);
  }
  for (auto i = 0; i < 3; i++) {
    close(i);
  }

  free(cmdline);
}

int main(int argc, char **argv) {
  Options opts;
  opts.reset();
  ParseCommandLine(argc, argv, opts);
  if (opts.daemon_mode) {
    ClassifyDaemon(opts);
  } else {
    auto index_data = load_index(opts);
    classify(opts, index_data);
    delete std::get<2>(index_data);
  }

  return 0;
}

void ReportStats(struct timeval time1, struct timeval time2,
  ClassificationStats &stats)
{
  time2.tv_usec -= time1.tv_usec;
  time2.tv_sec -= time1.tv_sec;
  if (time2.tv_usec < 0) {
    time2.tv_sec--;
    time2.tv_usec += 1000000;
  }
  double seconds = time2.tv_usec;
  seconds /= 1e6;
  seconds += time2.tv_sec;

  uint64_t total_unclassified = stats.total_sequences - stats.total_classified;

  if (isatty(fileno(stderr)))
    cerr << "\r";
  fprintf(stderr,
          "%llu sequences (%.2f Mbp) processed in %.3fs (%.1f Kseq/m, %.2f Mbp/m).\n",
          (unsigned long long) stats.total_sequences,
          stats.total_bases / 1.0e6,
          seconds,
          stats.total_sequences / 1.0e3 / (seconds / 60),
          stats.total_bases / 1.0e6 / (seconds / 60) );
  fprintf(stderr, "  %llu sequences classified (%.2f%%)\n",
          (unsigned long long) stats.total_classified,
          stats.total_classified * 100.0 / stats.total_sequences);
  fprintf(stderr, "  %llu sequences unclassified (%.2f%%)\n",
          (unsigned long long) total_unclassified,
          total_unclassified * 100.0 / stats.total_sequences);
}

void ProcessFiles(const char *filename1, const char *filename2,
    KeyValueStore *hash, Taxonomy &tax,
    IndexOptions &idx_opts, Options &opts, ClassificationStats &stats,
    OutputStreamData &outputs,
    taxon_counters_t &total_taxon_counters)
{
  // The priority queue for output is designed to ensure fragment data
  // is output in the same order it was input
  auto comparator = [](const OutputData &a, const OutputData &b) {
    return a.block_id > b.block_id;
  };
  std::priority_queue<OutputData, vector<OutputData>, decltype(comparator)>
    output_queue(comparator);
  uint64_t next_input_block_id = 0;
  uint64_t next_output_block_id = 0;
  omp_lock_t output_lock;
  omp_init_lock(&output_lock);
  BatchSequenceReader r1(filename1);
  BatchSequenceReader r2(filename2);
  std::vector<OutputData> buffers(omp_get_max_threads() * 2);

  #pragma omp parallel
  {
    MinimizerScanner scanner(idx_opts.k, idx_opts.l, idx_opts.spaced_seed_mask,
                             idx_opts.dna_db, idx_opts.toggle_mask,
                             idx_opts.revcom_version);
    vector<taxid_t> taxa;
    taxon_counts_t hit_counts;
    ostringstream kraken_oss, c1_oss, c2_oss, u1_oss, u2_oss;
    ClassificationStats thread_stats = {0, 0, 0};
    vector<string> translated_frames(6);
    Sequence *seq1 = nullptr, *seq2 = nullptr;
    BatchSequenceReader reader1(r1), reader2(r2);
    uint64_t block_id;
    OutputData out_data;
    taxon_counters_t thread_taxon_counters;

    while (true) {
      thread_stats.total_sequences = 0;
      thread_stats.total_bases = 0;
      thread_stats.total_classified = 0;

      auto ok_read = false;

      #pragma omp critical(seqread)
      {  // Input processing block
        if (! opts.paired_end_processing) {
          // Unpaired data?  Just read in a sized block
          ok_read = reader1.LoadBlock((size_t)(3 * 1024 * 1024));
        }
        else if (! opts.single_file_pairs) {
          // Paired data in 2 files?  Read a line-counted batch from each file.
          ok_read = reader1.LoadBatch(NUM_FRAGMENTS_PER_THREAD);
          if (ok_read && opts.paired_end_processing)
            ok_read = reader2.LoadBatch(NUM_FRAGMENTS_PER_THREAD);
        }
        else {
          auto frags = NUM_FRAGMENTS_PER_THREAD * 2;
          // Ensure frag count is even - just in case above line is changed
          if (frags % 2 == 1)
            frags++;
          ok_read = reader1.LoadBatch(frags);
        }
        block_id = next_input_block_id++;
      }

      if (! ok_read)
        break;

      // Reset all dynamically-growing things
      kraken_oss.str("");
      c1_oss.str("");
      c2_oss.str("");
      u1_oss.str("");
      u2_oss.str("");
      thread_taxon_counters.clear();

      while ((seq1 = reader1.NextSequence()) != NULL) {
        auto valid_fragment = true;
        // auto valid_fragment = (seq1 = reader1.NextSequence()) != nullptr;
        if (opts.paired_end_processing && valid_fragment) {
          if (opts.single_file_pairs) {
            seq2 = reader1.NextSequence();
            valid_fragment = seq2 != nullptr;
          } else {
            seq2 = reader2.NextSequence();
            valid_fragment = seq2 != nullptr;
          }
        }
        if (! valid_fragment)
          break;
        thread_stats.total_sequences++;
        if (opts.minimum_quality_score > 0) {
          MaskLowQualityBases(*seq1, opts.minimum_quality_score);
          if (opts.paired_end_processing)
            MaskLowQualityBases(*seq2, opts.minimum_quality_score);
        }
        taxid_t call;
        if (opts.paired_end_processing) {
          call =
              ClassifySequence(*seq1, *seq2, kraken_oss, hash, tax, idx_opts,
                               opts, thread_stats, scanner, taxa, hit_counts,
                               translated_frames, thread_taxon_counters);
        } else {
          auto empty_sequence = Sequence();
          call = ClassifySequence(*seq1, empty_sequence, kraken_oss, hash, tax, idx_opts,
                                  opts, thread_stats, scanner, taxa, hit_counts,
                                  translated_frames, thread_taxon_counters);
        }
        if (call) {
          char buffer[1024] = "";
          sprintf(buffer, " kraken:taxid|%llu",
              (unsigned long long) tax.nodes()[call].external_id);
          seq1->header += buffer;
          c1_oss << seq1->to_string();
          if (opts.paired_end_processing) {
            seq2->header += buffer;
            c2_oss << seq2->to_string();
          }
        }
        else {
          u1_oss << seq1->to_string();
          if (opts.paired_end_processing)
            u2_oss << seq2->to_string();
        }
        thread_stats.total_bases += seq1->seq.size();
        if (opts.paired_end_processing)
          thread_stats.total_bases += seq2->seq.size();
      }

      // #pragma omp atomic
      // #pragma omp atomic
      // #pragma omp atomic

      #pragma omp critical(output_stats)
      {
        stats.total_bases += thread_stats.total_bases;
        stats.total_sequences += thread_stats.total_sequences;
        stats.total_classified += thread_stats.total_classified;

        if (isatty(fileno(stderr)))
          cerr << "\rProcessed " << stats.total_sequences
               << " sequences (" << stats.total_bases << " bp) ...";
      }

      if (!outputs.initialized) {
        InitializeOutputs(opts, outputs, reader1.file_format());
      }

      out_data.block_id = block_id;
      out_data.kraken_str.assign(kraken_oss.str());
      out_data.classified_out1_str.assign(c1_oss.str());
      out_data.classified_out2_str.assign(c2_oss.str());
      out_data.unclassified_out1_str.assign(u1_oss.str());
      out_data.unclassified_out2_str.assign(u2_oss.str());

      #pragma omp critical(output_queue)
      {
        output_queue.push(std::move(out_data));
      }

      if (!opts.report_filename.empty()) {
#pragma omp critical(update_taxon_counters)
        for (auto &kv_pair : thread_taxon_counters) {
          total_taxon_counters[kv_pair.first] += std::move(kv_pair.second);
        }
      }

      bool output_loop = true;
      bool borrowed_buffer = false;
      while (output_loop) {
        #pragma omp critical(output_queue)
        {
          output_loop = !output_queue.empty();
          if (!output_loop && !borrowed_buffer) {
            out_data = std::move(buffers.back());
            buffers.pop_back();
          } else if (borrowed_buffer && output_loop) {
            buffers.push_back(std::move(out_data));
          }

          if (output_loop) {
            if (output_queue.top().block_id == next_output_block_id) {
              out_data = std::move(const_cast<OutputData &>(output_queue.top()));
              output_queue.pop();
              borrowed_buffer = true;
              // Acquiring output lock obligates thread to print out
              // next output data block, contained in out_data
              omp_set_lock(&output_lock);
              next_output_block_id++;
            } else {
              output_loop = false;
              if (buffers.size() > 0) {
                out_data = std::move(buffers.back());
                buffers.pop_back();
              } else {
                out_data = OutputData();
              }
            }
          }
        }
        if (! output_loop)
          break;
        if (outputs.kraken_output != nullptr)
          (*outputs.kraken_output) << out_data.kraken_str;
        if (outputs.classified_output1 != nullptr)
          (*outputs.classified_output1) << out_data.classified_out1_str;
        if (outputs.classified_output2 != nullptr)
          (*outputs.classified_output2) << out_data.classified_out2_str;
        if (outputs.unclassified_output1 != nullptr)
          (*outputs.unclassified_output1) << out_data.unclassified_out1_str;
        if (outputs.unclassified_output2 != nullptr)
          (*outputs.unclassified_output2) << out_data.unclassified_out2_str;
        omp_unset_lock(&output_lock);
      }  // end while output loop
    } // end while
  } // end parallel block

  omp_destroy_lock(&output_lock);
  if (outputs.kraken_output != nullptr)
    (*outputs.kraken_output) << std::flush;
  if (outputs.classified_output1 != nullptr)
    (*outputs.classified_output1) << std::flush;
  if (outputs.classified_output2 != nullptr)
    (*outputs.classified_output2) << std::flush;
  if (outputs.unclassified_output1 != nullptr)
    (*outputs.unclassified_output1) << std::flush;
  if (outputs.unclassified_output2 != nullptr)
    (*outputs.unclassified_output2) << std::flush;
}

taxid_t ResolveTree(taxon_counts_t &hit_counts,
    Taxonomy &taxonomy, size_t total_minimizers, Options &opts)
{
  taxid_t max_taxon = 0;
  uint32_t max_score = 0;
  uint32_t required_score = ceil(opts.confidence_threshold * total_minimizers);

  // Sum each taxon's LTR path, find taxon with highest LTR score
  for (auto &kv_pair : hit_counts) {
    taxid_t taxon = kv_pair.first;
    uint32_t score = 0;

    for (auto &kv_pair2 : hit_counts) {
      taxid_t taxon2 = kv_pair2.first;

      if (taxonomy.IsAAncestorOfB(taxon2, taxon)) {
        score += kv_pair2.second;
      }
    }

    if (score > max_score) {
      max_score = score;
      max_taxon = taxon;
    }
    else if (score == max_score) {
      max_taxon = taxonomy.LowestCommonAncestor(max_taxon, taxon);
    }
  }

  // Reset max. score to be only hits at the called taxon
  max_score = hit_counts[max_taxon];
  // We probably have a call w/o required support (unless LCA resolved tie)
  while (max_taxon && max_score < required_score) {
    max_score = 0;
    for (auto &kv_pair : hit_counts) {
      taxid_t taxon = kv_pair.first;
      // Add to score if taxon in max_taxon's clade
      if (taxonomy.IsAAncestorOfB(max_taxon, taxon)) {
        max_score += kv_pair.second;
      }
    }
    // Score is now sum of hits at max_taxon and w/in max_taxon clade
    if (max_score >= required_score)
      // Kill loop and return, we've got enough support here
      return max_taxon;
    else
      // Run up tree until confidence threshold is met
      // Run off tree if required score isn't met
      max_taxon = taxonomy.nodes()[max_taxon].parent_id;
  }

  return max_taxon;
}

std::string TrimPairInfo(std::string &id) {
  size_t sz = id.size();
  if (sz <= 2)
    return id;
  if ( id[sz - 2] == '/' && (id[sz - 1] == '1' || id[sz - 1] == '2') )
    return id.substr(0, sz - 2);
  return id;
}

taxid_t ClassifySequence(Sequence &dna, Sequence &dna2, ostringstream &koss,
                         KeyValueStore *hash, Taxonomy &taxonomy,
                         IndexOptions &idx_opts, Options &opts,
                         ClassificationStats &stats, MinimizerScanner &scanner,
                         vector<taxid_t> &taxa, taxon_counts_t &hit_counts,
                         vector<string> &tx_frames,
                         taxon_counters_t &curr_taxon_counts)
{
  uint64_t *minimizer_ptr;
  taxid_t call = 0;
  taxa.clear();
  hit_counts.clear();
  auto frame_ct = opts.use_translated_search ? 6 : 1;
  int64_t minimizer_hit_groups = 0;

  for (int mate_num = 0; mate_num < 2; mate_num++) {
    if (mate_num == 1 && ! opts.paired_end_processing)
      break;

    if (opts.use_translated_search) {
      TranslateToAllFrames(mate_num == 0 ? dna.seq : dna2.seq, tx_frames);
    }
    // index of frame is 0 - 5 w/ tx search (or 0 if no tx search)
    for (int frame_idx = 0; frame_idx < frame_ct; frame_idx++) {
      if (opts.use_translated_search) {
        scanner.LoadSequence(tx_frames[frame_idx]);
      }
      else {
        scanner.LoadSequence(mate_num == 0 ? dna.seq : dna2.seq);
      }
      uint64_t last_minimizer = UINT64_MAX;
      taxid_t last_taxon = TAXID_MAX;
      while ((minimizer_ptr = scanner.NextMinimizer()) != nullptr) {
        taxid_t taxon;
        if (scanner.is_ambiguous()) {
          taxon = AMBIGUOUS_SPAN_TAXON;
        }
        else {
          if (*minimizer_ptr != last_minimizer) {
            bool skip_lookup = false;
            if (idx_opts.minimum_acceptable_hash_value) {
              if (MurmurHash3(*minimizer_ptr) < idx_opts.minimum_acceptable_hash_value)
                skip_lookup = true;
            }
            taxon = 0;
            if (! skip_lookup)
              taxon = hash->Get(*minimizer_ptr);
            last_taxon = taxon;
            last_minimizer = *minimizer_ptr;
            // Increment this only if (a) we have DB hit and
            // (b) minimizer != last minimizer
            if (taxon) {
              minimizer_hit_groups++;
              // New minimizer should trigger registering minimizer in RC/HLL
              if (!opts.report_filename.empty()) {
                curr_taxon_counts[taxon].add_kmer(scanner.last_minimizer());
              }
            }
          }
          else {
            taxon = last_taxon;
          }
          if (taxon) {
            if (opts.quick_mode && minimizer_hit_groups >= opts.minimum_hit_groups) {
              call = taxon;
              goto finished_searching;  // need to break 3 loops here
            }
            hit_counts[taxon]++;
          }
        }
        taxa.push_back(taxon);
      }
      if (opts.use_translated_search && frame_idx != 5)
        taxa.push_back(READING_FRAME_BORDER_TAXON);
    }
    if (opts.paired_end_processing && mate_num == 0)
      taxa.push_back(MATE_PAIR_BORDER_TAXON);
  }

  finished_searching:

  auto total_kmers = taxa.size();
  if (opts.paired_end_processing)
    total_kmers--;  // account for the mate pair marker
  if (opts.use_translated_search)  // account for reading frame markers
    total_kmers -= opts.paired_end_processing ? 4 : 2;
  call = ResolveTree(hit_counts, taxonomy, total_kmers, opts);
  // Void a call made by too few minimizer groups
  if (call && minimizer_hit_groups < opts.minimum_hit_groups)
    call = 0;

  if (call) {
    stats.total_classified++;
    if (!opts.report_filename.empty())
      curr_taxon_counts[call].incrementReadCount();
  }

  if (call)
    koss << "C\t";
  else
    koss << "U\t";
  if (! opts.paired_end_processing)
    koss << dna.header << "\t";
  else
    koss << TrimPairInfo(dna.header) << "\t";

  auto ext_call = taxonomy.nodes()[call].external_id;
  if (opts.print_scientific_name) {
    const char *name = nullptr;
    if (call) {
      name = taxonomy.name_data() + taxonomy.nodes()[call].name_offset;
    }
    koss << (name ? name : "unclassified") << " (taxid " << ext_call << ")";
  }
  else {
    koss << ext_call;
  }

  koss << "\t";
  if (! opts.paired_end_processing)
    koss << dna.seq.size() << "\t";
  else
    koss << dna.seq.size() << "|" << dna2.seq.size() << "\t";

  if (opts.quick_mode) {
    koss << ext_call << ":Q";
  }
  else {
    if (taxa.empty())
      koss << "0:0";
    else
      AddHitlistString(koss, taxa, taxonomy);
  }

  koss << endl;

  return call;
}

void AddHitlistString(ostringstream &oss, vector<taxid_t> &taxa,
    Taxonomy &taxonomy)
{
  auto last_code = taxa[0];
  auto code_count = 1;

  for (size_t i = 1; i < taxa.size(); i++) {
    auto code = taxa[i];

    if (code == last_code) {
      code_count += 1;
    }
    else {
      if (last_code != MATE_PAIR_BORDER_TAXON && last_code != READING_FRAME_BORDER_TAXON) {
        if (last_code == AMBIGUOUS_SPAN_TAXON) {
          oss << "A:" << code_count << " ";
        }
        else {
          auto ext_code = taxonomy.nodes()[last_code].external_id;
          oss << ext_code << ":" << code_count << " ";
        }
      }
      else {  // mate pair/reading frame marker
        oss << (last_code == MATE_PAIR_BORDER_TAXON ? "|:| " : "-:- ");
      }
      code_count = 1;
      last_code = code;
    }
  }
  if (last_code != MATE_PAIR_BORDER_TAXON && last_code != READING_FRAME_BORDER_TAXON) {
    if (last_code == AMBIGUOUS_SPAN_TAXON) {
      oss << "A:" << code_count << " ";
    }
    else {
      auto ext_code = taxonomy.nodes()[last_code].external_id;
      oss << ext_code << ":" << code_count;
    }
  }
  else {  // mate pair/reading frame marker
    oss << (last_code == MATE_PAIR_BORDER_TAXON ? "|:|" : "-:-");
  }
}

ofstream *OpenOutputStream(const std::string &filename) {
  ofstream *out = new ofstream();
  out->exceptions(std::ios::badbit | std::ios::failbit);
  try {
    out->open(filename);
  } catch(std::exception &e) {
    std::cerr << '\r' << "Unable to open file: " << filename
              << ", reason: " << strerror(errno) << std::endl;
    delete out;
    exit(EXIT_FAILURE);
  }

  return out;
}

void InitializeOutputs(Options &opts, OutputStreamData &outputs, SequenceFormat format) {
  #pragma omp critical(output_init)
  {
    if (! outputs.initialized) {
      if (! opts.classified_output_filename.empty()) {
        if (opts.paired_end_processing) {
          vector<string> fields = SplitString(opts.classified_output_filename, "#", 3);
          if (fields.size() < 2) {
            errx(EX_DATAERR, "Paired filename format missing # character: %s",
                 opts.classified_output_filename.c_str());
          }
          else if (fields.size() > 2) {
            errx(EX_DATAERR, "Paired filename format has >1 # character: %s",
                 opts.classified_output_filename.c_str());
          }
          outputs.classified_output1 = OpenOutputStream(fields[0] + "_1" + fields[1]);
          outputs.classified_output2 = OpenOutputStream(fields[0] + "_2" + fields[1]);
        }
        else
          outputs.classified_output1 = new ofstream(opts.classified_output_filename);
        outputs.printing_sequences = true;
      }
      if (! opts.unclassified_output_filename.empty()) {
        if (opts.paired_end_processing) {
          vector<string> fields = SplitString(opts.unclassified_output_filename, "#", 3);
          if (fields.size() < 2) {
            errx(EX_DATAERR, "Paired filename format missing # character: %s",
                 opts.unclassified_output_filename.c_str());
          }
          else if (fields.size() > 2) {
            errx(EX_DATAERR, "Paired filename format has >1 # character: %s",
                 opts.unclassified_output_filename.c_str());
          }
          outputs.unclassified_output1 = OpenOutputStream(fields[0] + "_1" + fields[1]);
          outputs.unclassified_output2 = OpenOutputStream(fields[0] + "_2" + fields[1]);
        }
        else
          outputs.unclassified_output1 = new ofstream(opts.unclassified_output_filename);
        outputs.printing_sequences = true;
      }
      if (!opts.kraken_output_filename.empty()) {
        if (opts.kraken_output_filename == "-")  // Special filename to silence Kraken output
          outputs.kraken_output = nullptr;
        else {
          // outputs.kraken_output = new ofstream(opts.kraken_output_filename);
          outputs.kraken_output = OpenOutputStream(opts.kraken_output_filename);
        }
      }
      outputs.initialized = true;
    }
  }
}

void MaskLowQualityBases(Sequence &dna, int minimum_quality_score) {
  if (dna.format != FORMAT_FASTQ)
    return;
  if (dna.seq.size() != dna.quals.size())
    errx(EX_DATAERR, "%s: Sequence length (%d) != Quality string length (%d)",
                     dna.header.c_str(), (int) dna.seq.size(), (int) dna.quals.size());
  for (size_t i = 0; i < dna.seq.size(); i++) {
    if ((dna.quals[i] - '!') < minimum_quality_score)
      dna.seq[i] = 'x';
  }
}

void ParseCommandLine(int argc, char **argv, Options &opts) {
  int opt;

  while ((opt = getopt(argc, argv, "h?H:t:o:T:p:R:C:U:O:Q:g:nmzqPSMKD")) != -1) {
    switch (opt) {
      case 'h' : case '?' :
        usage(0);
        break;
      case 'H' :
        opts.index_filename = optarg;
        break;
      case 't' :
        opts.taxonomy_filename = optarg;
        break;
      case 'T' :
        opts.confidence_threshold = std::stod(optarg);
        if (opts.confidence_threshold < 0 || opts.confidence_threshold > 1) {
          errx(EX_USAGE, "confidence threshold must be in [0, 1]");
        }
        break;
      case 'o' :
        opts.options_filename = optarg;
        break;
      case 'q' :
        opts.quick_mode = true;
        break;
      case 'p' :
        opts.num_threads = atoi(optarg);
        if (opts.num_threads < 1)
          errx(EX_USAGE, "number of threads can't be less than 1");
        break;
      case 'g' :
        opts.minimum_hit_groups = atoi(optarg);
        break;
      case 'P' :
        opts.paired_end_processing = true;
        break;
      case 'S' :
        opts.paired_end_processing = true;
        opts.single_file_pairs = true;
        break;
      case 'm' :
        opts.mpa_style_report = true;
        break;
      case 'K':
        opts.report_kmer_data = true;
        break;
      case 'R' :
        opts.report_filename = optarg;
        break;
      case 'z' :
        opts.report_zero_counts = true;
        break;
      case 'C' :
        opts.classified_output_filename = optarg;
        break;
      case 'U' :
        opts.unclassified_output_filename = optarg;
        break;
      case 'O' :
        opts.kraken_output_filename = optarg;
        break;
      case 'n' :
        opts.print_scientific_name = true;
        break;
      case 'Q' :
        opts.minimum_quality_score = atoi(optarg);
        break;
      case 'M' :
        opts.use_memory_mapping = true;
        break;
      case 'D':
        opts.daemon_mode = true;
        break;
    }
  }

  if (opts.index_filename.empty() ||
      opts.taxonomy_filename.empty() ||
      opts.options_filename.empty())
  {
    warnx("mandatory filename missing");
    usage();
  }

  if (opts.mpa_style_report && opts.report_filename.empty()) {
    warnx("-m requires -R be used");
    usage();
  }

  for (int i = optind; i < argc; i++) {
    opts.filenames.push_back(argv[i]);
  }

  optind = 1;
}

void usage(int exit_code) {
  cerr << "Usage: classify [options] <fasta/fastq file(s)>" << endl
       << endl
       << "Options: (*mandatory)" << endl
       << "* -H filename      Kraken 2 index filename" << endl
       << "* -t filename      Kraken 2 taxonomy filename" << endl
       << "* -o filename      Kraken 2 options filename" << endl
       << "  -q               Quick mode" << endl
       << "  -M               Use memory mapping to access hash & taxonomy" << endl
       << "  -T NUM           Confidence score threshold (def. 0)" << endl
       << "  -p NUM           Number of threads (def. 1)" << endl
       << "  -Q NUM           Minimum quality score (FASTQ only, def. 0)" << endl
       << "  -P               Process pairs of reads" << endl
       << "  -S               Process pairs with mates in same file" << endl
       << "  -R filename      Print report to filename" << endl
       << "  -m               In comb. w/ -R, use mpa-style report" << endl
       << "  -z               In comb. w/ -R, report taxa w/ 0 count" << endl
       << "  -n               Print scientific name instead of taxid in Kraken output" << endl
       << "  -g NUM           Minimum number of hit groups needed for call" << endl
       << "  -C filename      Filename/format to have classified sequences" << endl
       << "  -U filename      Filename/format to have unclassified sequences" << endl
       << "  -O filename      Output file for normal Kraken output" << endl
       << "  -K               In comb. w/ -R, provide minimizer information in report" << endl
       << "  -D               Start a daemon, this options is intended to be used with wrappers" << std::endl;
  exit(exit_code);
}
