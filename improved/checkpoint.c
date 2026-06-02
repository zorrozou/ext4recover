/*
 * checkpoint.c - Checkpoint/resume support for ext4recover
 */

#include "ext4_common_v5.h"

/*
 * Save checkpoint state to file
 */
int save_checkpoint(struct recover_context *ctx)
{
    FILE *fp = fopen(ctx->checkpoint_file, "w");
    if (!fp) {
        LOG_ERROR("Failed to create checkpoint file: %s", ctx->checkpoint_file);
        return -1;
    }
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"version\": \"%s\",\n", VERSION);
    fprintf(fp, "  \"device\": \"%s\",\n", ctx->device_name);
    fprintf(fp, "  \"files_recovered\": %lu,\n", ctx->files_recovered);
    fprintf(fp, "  \"journal_offset\": %llu,\n", 
            (unsigned long long)ctx->checkpoint_journal_offset);
    fprintf(fp, "  \"timestamp\": %ld\n", time(NULL));
    fprintf(fp, "}\n");
    
    fclose(fp);
    LOG_DEBUG(ctx, "Checkpoint saved: %lu files, journal offset %llu",
              ctx->files_recovered, 
              (unsigned long long)ctx->checkpoint_journal_offset);
    return 0;
}

/*
 * Load checkpoint state from file
 */
int load_checkpoint(struct recover_context *ctx)
{
    FILE *fp = fopen(ctx->checkpoint_file, "r");
    if (!fp) {
        LOG_INFO("No checkpoint file found, starting fresh");
        return -1;
    }
    
    char line[512];
    unsigned long files = 0;
    unsigned long long offset = 0;
    int found_files = 0, found_offset = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "  \"files_recovered\": %lu,", &files) == 1) {
            found_files = 1;
        }
        if (sscanf(line, "  \"journal_offset\": %llu,", &offset) == 1) {
            found_offset = 1;
        }
    }
    
    fclose(fp);
    
    if (found_files && found_offset) {
        ctx->checkpoint_files_recovered = files;
        ctx->checkpoint_journal_offset = offset;
        LOG_INFO("Checkpoint loaded: resuming from %lu files, journal offset %llu",
                 files, offset);
        return 0;
    }
    
    LOG_WARN("Checkpoint file corrupted, starting fresh");
    return -1;
}

/*
 * Clear checkpoint file
 */
void clear_checkpoint(struct recover_context *ctx)
{
    if (access(ctx->checkpoint_file, F_OK) == 0) {
        unlink(ctx->checkpoint_file);
        LOG_DEBUG(ctx, "Checkpoint file cleared");
    }
}
