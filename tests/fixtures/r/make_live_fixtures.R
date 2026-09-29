# Random R data for tests/verify_suite/v135_r_live.do (R-READ-1), written at
# test time: Rscript make_live_fixtures.R <stem>
# Seeded, so every run writes the same values. Writes <stem>_gz.rds,
# _none.rds, _v2.rds, _zstd.rds (when this R writes zstd), _ws.RData (beside a
# function), _big.rds (3,000,000 rows) and the oracles _oracle.json,
# _i64.json and _big.json from the same objects. Base R and jsonlite only.
args <- commandArgs(TRUE)
stem <- args[1]
if (!requireNamespace("jsonlite", quietly = TRUE)) {
  cat("NOJSONLITE\n")
  quit(status = 0)
}
set.seed(20260928)
n <- 50000
tagged_na <- function(tag) {
  b <- writeBin(NA_real_, raw(), endian = "little")
  b[5] <- as.raw(utf8ToInt(tag))
  readBin(b, "double", endian = "little")
}
na_tag_of <- function(x) as.integer(writeBin(x, raw(), endian = "little")[5])
num <- function(x) vapply(x, function(v) {
  if (is.na(v) && !is.nan(v)) {
    t <- na_tag_of(v)
    if (t != 0) paste0("NA(", intToUtf8(t), ")") else NA_character_
  } else if (is.nan(v)) "NaN"
  else if (is.infinite(v)) (if (v > 0) "Inf" else "-Inf")
  else if (v == 0 && 1 / v < 0) "-0"
  else sprintf("%.17g", v)
}, "", USE.NAMES = FALSE)
pick <- function(p) sample.int(n, max(1, round(n * p)))

ints <- sample(c(-2147483647L, 2147483647L, -1L, 0L, 1L, sample.int(1e6, 100)), n, TRUE)
ints[pick(.05)] <- NA
dbl <- c(rnorm(n - 9), 1e308, -1e308, 5e-324, -0, NaN, Inf, -Inf, 2^53 + 2, 0.1)[sample.int(n)]
dbl[pick(.05)] <- NA
tag <- rnorm(n)
k <- pick(.1)
tag[k] <- vapply(sample(letters, length(k), TRUE), tagged_na, 0)
tag[pick(.02)] <- NA
chars <- c("", "a", "é", "中文", "\U0001F600 emoji", "tab\there", "new\nline",
           "quote\"s", strrep("long", 700))
txt <- sample(chars, n, TRUE)
txt[pick(.05)] <- NA
lg <- sample(c(TRUE, FALSE, NA), n, TRUE)
fac <- factor(sample(sprintf("level %03d", 1:300), n, TRUE))
day <- as.Date("1970-01-01") + sample(-200000:200000, n, TRUE)
day[pick(.05)] <- NA
fday <- structure(as.numeric(day) + round(runif(n), 6), class = "Date")
when <- as.POSIXct(round(runif(n, -3e9, 4e9), 6), origin = "1970-01-01", tz = "UTC")
when[pick(.05)] <- NA
clock <- structure(round(runif(n, 0, 86399.999), 3), units = "secs", class = c("hms", "difftime"))
# integer64: two 32-bit words per value, across the whole range
lo <- sample.int(2^31 - 1, n, TRUE) - 2^30
hi <- sample(c(-2^31 + 1, -1, 0, 1, 2^21, 2^31 - 1), n, TRUE)
i64 <- readBin(writeBin(as.integer(rbind(lo, hi)), raw(), size = 4, endian = "little"),
               "double", n = n, endian = "little")
class(i64) <- "integer64"
df <- data.frame(ints = ints, dbl = dbl, tag = tag, txt = txt, lg = lg, fac = fac, day = day,
                 fday = fday, when = when, clock = clock, stringsAsFactors = FALSE)
df[["i64"]] <- i64
saveRDS(df, paste0(stem, "_gz.rds"))
saveRDS(df, paste0(stem, "_none.rds"), compress = FALSE)
saveRDS(df, paste0(stem, "_v2.rds"), version = 2)
zstd <- tryCatch({ saveRDS(df, paste0(stem, "_zstd.rds"), compress = "zstd"); TRUE },
                 error = function(e) FALSE)
g <- function(x) x + 1
invisible(g(1))
save(g, df, file = paste0(stem, "_ws.RData"))
oracle <- list(n = n, zstd = zstd, ints = ints, dbl = num(dbl), tag = num(tag), txt = txt, lg = lg,
               fac = as.integer(fac), levels = levels(fac), day = num(unclass(day)),
               fday = num(unclass(fday)), when = num(unclass(when)), clock = num(unclass(clock)))
writeLines(jsonlite::toJSON(oracle, auto_unbox = TRUE, na = "null", digits = NA),
           paste0(stem, "_oracle.json"), useBytes = TRUE)
writeLines(jsonlite::toJSON(list(hi = hi, lo = lo), digits = NA), paste0(stem, "_i64.json"))

# a large frame, out of core
N <- 3000000
big <- data.frame(id = 1:N, x = (1:N) / 7, g = factor(sample(letters, N, TRUE)), s = as.character(1:N))
saveRDS(big, paste0(stem, "_big.rds"))
writeLines(jsonlite::toJSON(list(n = N, g_a = sum(big[["g"]] == "a"), s_last = big[["s"]][N]),
                            auto_unbox = TRUE), paste0(stem, "_big.json"))
cat("DONE\n")
