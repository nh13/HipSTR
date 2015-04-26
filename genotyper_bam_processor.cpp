#include <iostream>

#include "extract_indels.h"
#include "genotyper_bam_processor.h"


void GenotyperBamProcessor::analyze_reads_and_phasing(std::vector< std::vector<BamTools::BamAlignment> >& alignments,
						      std::vector< std::vector<double> >& log_p1s,
						      std::vector< std::vector<double> >& log_p2s,
						      std::vector<std::string>& rg_names, Region& region, std::string& ref_allele, std::string& chrom_seq){ 
  std::cerr << alignments.size() << " " << log_p1s.size() << " " << log_p2s.size() << " " << rg_names.size() << std::endl;
  assert(alignments.size() == log_p1s.size() && alignments.size() == log_p2s.size() && alignments.size() == rg_names.size());
  std::vector< std::vector<int> > str_bp_lengths(alignments.size());
  std::vector< std::vector<double> > str_log_p1s(alignments.size()), str_log_p2s(alignments.size());
  int inf_reads = 0;

  // Extract bp differences and phasing probabilities for each read if we 
  // need to utilize the length-based EM genotyper for stutter model training or genotyping
  if (!read_stutter_models_ || !use_seq_aligner_){
    for (unsigned int i = 0; i < alignments.size(); ++i){
      for (unsigned int j = 0; j < alignments[i].size(); ++j){
	int bp_diff;
	bool got_size = ExtractCigar(alignments[i][j].CigarData, alignments[i][j].Position, region.start()-region.period(), region.stop()+region.period(), bp_diff);
	if (got_size){
	  if (bp_diff < -(int)(region.stop()-region.start()+1)) {
	    std::cerr << "WARNING: Excluding read with bp difference greater than reference allele: " << alignments[i][j].Name << std::endl;
	    continue;
	  }
	  inf_reads++;
	  str_bp_lengths[i].push_back(bp_diff);
	  if (log_p1s.size() == 0){
	    str_log_p1s[i].push_back(0); str_log_p2s[i].push_back(0); // Assign equal phasing LLs as no SNP info is available
	  }
	  else {
	    str_log_p1s[i].push_back(log_p1s[i][j]); str_log_p2s[i].push_back(log_p2s[i][j]);
	  }
	}
      }
    }
  }


  bool trained = false;
  StutterModel* stutter_model          = NULL;
  EMStutterGenotyper* length_genotyper = NULL;;
  if (read_stutter_models_){
    // Attempt to extact model from dictionary
    auto model_iter = stutter_models_.find(region);
    if (model_iter != stutter_models_.end()){
      stutter_model = model_iter->second->copy();
    }
    else
      std::cerr << "WARNING: No stutter model found for " << region.chrom() << ":" << region.start() << "-" << region.stop() << std::endl;
  }
  else {
    // Learn stutter model using length-based EM algorithm
    std::cerr << "Building EM stutter genotyper" << std::endl;
    length_genotyper = new EMStutterGenotyper(region.chrom(), region.start(), region.stop(), str_bp_lengths, str_log_p1s, str_log_p2s, rg_names, region.period(), 0);
    std::cerr << "Training EM stutter genotyper" << std::endl;
    std::cerr << FRAC_LL_CONVERGE << std::endl;
    trained = length_genotyper->train(MAX_EM_ITER, ABS_LL_CONVERGE, FRAC_LL_CONVERGE);
    if (trained){
      if (output_stutter_models_)
	length_genotyper->get_stutter_model()->write_model(region.chrom(), region.start(), region.stop(), stutter_model_out_);
      num_em_converge_++;
      stutter_model = length_genotyper->get_stutter_model()->copy();
      std::cerr << "Learned stutter model: " << *stutter_model << std::endl;
    }
    else {
      num_em_fail_++;
      std::cerr << "Stutter model training failed for locus " << region.chrom() << ":" << region.start() << "-" << region.stop() 
		<< " with " << inf_reads << " informative reads" << std::endl;
    }
  }
  
  if (stutter_model != NULL) {
    if (use_seq_aligner_){
      // Use sequence-based genotyper
      SeqStutterGenotyper seq_genotyper(region, alignments, log_p1s, log_p2s, rg_names, chrom_seq, *stutter_model, (have_ref_vcf_ ? &ref_vcf_ : NULL));
      if (seq_genotyper.genotype()) {
	num_genotype_success_++;
	if (output_str_gts_)
	  seq_genotyper.write_vcf_record(samples_to_genotype_, str_vcf_);
      }
      else
	num_genotype_fail_++;
    }
    else {
      // Use length-based genotyper
      if (length_genotyper == NULL){
	length_genotyper = new EMStutterGenotyper(region.chrom(), region.start(), region.stop(), str_bp_lengths, str_log_p1s, str_log_p2s, rg_names, region.period(), 0);
	length_genotyper->set_stutter_model(*stutter_model);
      }
      bool use_pop_freqs = false;
      if (length_genotyper->genotype(use_pop_freqs)){
	num_genotype_success_++;
	if (output_str_gts_)
	  length_genotyper->write_vcf_record(ref_allele, samples_to_genotype_, str_vcf_);
      }
      else
	num_genotype_fail_++;
    }
  }
  delete stutter_model;
  delete length_genotyper;
}
 
