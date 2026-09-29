# Regenerate the R fixtures of tests/verify_suite/v133-v134 (R-READ-1).
#
#     Rscript make_r_fixtures.R          (base R and jsonlite; run from this folder)
#
# R itself writes every file here with saveRDS() and save(), and writes the
# oracle expected.json from the same objects, so the Stata tests compare
# parqit's reader against R's own reading of the data — values, classes,
# labels and the structures parqit must pass over. The content is fixed (no
# random numbers); the classes of haven, tibble, data.table, hms and bit64 are
# built from their attributes with structure(), so none of those packages is
# needed. Files:
#   frame.rds         gzip, serialization version 3: every column type parqit
#                     carries (see the columns below)
#   frame_v2.rds      the same data frame, version 2 (no ALTREP, no encoding
#                     in the header)
#   frame_zstd.rds    the same, zstd-compressed (R >= 4.5); frame_none.rds
#                     uncompressed
#   workspace.RData   save() of a byte-compiled function, an environment that
#                     holds itself, a list and two data frames
#   rownames.rds      character row names (six rows of mtcars)
#   edge.rds          repeated, missing and case-distinct names, list, matrix,
#                     complex and POSIXlt columns, ALTREP sequences and
#                     deferred as.character() of integers and of doubles
#   latin1.rds        strings marked latin1 and bytes
#   list.rds          an .rds holding a named list with two data frames
#   empty.rds         a data frame with no rows
#   refuse_xz.rds, refuse_bz2.rds, refuse_ascii.rds   formats parqit refuses

stopifnot(requireNamespace("jsonlite", quietly = TRUE))
here <- function(f) f

# ---- values R has no literal for -----------------------------------------
# haven's tagged NA: R's NA_real_ with a letter in the high word
tagged_na <- function(tag) {
  b <- writeBin(NA_real_, raw(), endian = "little")
  b[5] <- as.raw(utf8ToInt(tag))
  readBin(b, "double", endian = "little")
}
na_tag_of <- function(x) {
  b <- writeBin(x, raw(), endian = "little")
  t <- as.integer(b[5])
  if (is.na(x) && !is.nan(x) && t != 0) intToUtf8(t) else ""
}
# bit64::integer64 is an int64 held in the bits of a double
int64 <- function(hi, lo) {
  readBin(writeBin(c(as.integer(lo), as.integer(hi)), raw(), size = 4, endian = "little"),
          "double", n = 1, endian = "little")
}
i64_values <- c("9007199254740993", NA, "-5", "0", "123")
i64 <- structure(c(int64(2097152L, 1L), int64(NA_integer_, 0L), int64(-1L, -5L),
                   int64(0L, 0L), int64(0L, 123L)), class = "integer64")

# ---- the main data frame --------------------------------------------------
frame <- data.frame(
  i = c(1L, NA, 3L, -2147483647L, 2147483647L),
  d = c(1.5, NA, NaN, Inf, -1e300),
  s = c("x", NA, "é 中 \U0001F600", "", "a\"b"),
  l = c(TRUE, NA, FALSE, TRUE, FALSE),
  f = factor(c("lo", "hi", NA, "lo", "mid"), levels = c("lo", "mid", "hi")),
  o = factor(c("a", "b", "c", "a", "b"), ordered = TRUE),
  dt = as.Date(c("2020-03-01", NA, "1960-01-01", "1582-10-15", "2099-12-31")),
  ct = as.POSIXct(c("2020-03-01 12:34:56.789", NA, "1970-01-01 00:00:00",
                    "1960-01-01 00:00:00", "2038-01-19 03:14:08"), tz = "Europe/Lisbon"),
  dtm = as.difftime(c(1.5, 2, NA, 0, -3), units = "days"),
  stringsAsFactors = FALSE)
frame$hm <- structure(c(3661, 0, NA, 86399.5, 45), units = "secs", class = c("hms", "difftime"))
frame$idate <- structure(c(18322L, NA, 0L, -3653L, 47481L), class = c("IDate", "Date"))
frame$i64 <- i64
frame$lab <- structure(c(1, 98, tagged_na("a"), 2, 99), label = "Question one",
                       labels = c(Yes = 1, No = 2, Refused = 98, Unknown = 99, Skipped = tagged_na("a")),
                       na_values = c(98, 99),
                       class = c("haven_labelled_spss", "haven_labelled", "vctrs_vctr", "double"))
frame$score <- structure(c(0.5, 1, 2.5, NA, 1), labels = c(Half = 0.5, One = 1),
                         class = c("haven_labelled", "vctrs_vctr", "double"))
frame$sex <- structure(c("M", "F", "X", NA, "F"), label = "Sex",
                       labels = c(Male = "M", Female = "F", `Not stated` = "X"), na_values = "X",
                       class = c("haven_labelled_spss", "haven_labelled", "vctrs_vctr", "character"))
frame$stata_fmt <- structure(c(10, 20, 30, 40, 50), format.stata = "%9.2f", label = paste(rep("long", 30), collapse = " "))
comment(frame$i) <- c("first note on i", "second note")
attr(frame, "label") <- "Fixture data frame"
comment(frame) <- "a note on the data frame"
attr(frame, "source_info") <- list(made_by = "make_r_fixtures.R", version = 1L)

saveRDS(frame, here("frame.rds"))
saveRDS(frame, here("frame_v2.rds"), version = 2)
saveRDS(frame, here("frame_none.rds"), compress = FALSE)
have_zstd <- tryCatch({ saveRDS(frame, here("frame_zstd.rds"), compress = "zstd"); TRUE },
                      error = function(e) FALSE)
saveRDS(head(frame, 2), here("refuse_xz.rds"), compress = "xz")
saveRDS(head(frame, 2), here("refuse_bz2.rds"), compress = "bzip2")
saveRDS(head(frame, 2), here("refuse_ascii.rds"), ascii = TRUE)

# ---- a workspace -------------------------------------------------------------
f <- function(x) { y <- x + 1; y * 2 }
invisible(f(1)); invisible(f(2))           # the JIT compiles the body
f <- compiler::cmpfun(f)                   # ... and certainly now
e <- new.env()
assign("self", e, envir = e)
assign("v", 1:3, envir = e)
people <- data.frame(id = c(101L, 102L, 103L), name = c("Ana", "Rui", "Eva"), stringsAsFactors = FALSE)
towns <- data.frame(town = c("Porto", "Braga"), pop = c(231800, 193300), stringsAsFactors = FALSE)
lst <- list(a = 1, b = "two")
save(f, e, people, lst, towns, file = here("workspace.RData"))

# ---- character row names -----------------------------------------------------
rn <- head(mtcars[, c("mpg", "cyl", "wt")], 6)
saveRDS(rn, here("rownames.rds"))

# ---- edge cases ----------------------------------------------------------------
edge <- data.frame(a = 1:3, b = 4:6, c = 7:9, d = 10:12)
names(edge) <- c("x", "x", NA, "")
edge$X <- c("upper", "case", "name")
edge$lst <- list(1:2, "a", NULL)
edge$m <- matrix(1:6, 3)
edge$cx <- complex(real = 1:3, imaginary = 0)
edge$lt <- as.POSIXlt(c("2020-01-01", "2020-01-02", "2020-01-03"), tz = "UTC")
edge$seq <- 1:3                        # ALTREP compact sequence
edge$ds <- as.character(c(10L, NA, -3L)) # deferred as.character() of integers
edge$dr <- as.character(c(1.5, 2, 1e5))  # ... and of doubles
saveRDS(edge, here("edge.rds"))

# ---- encodings -----------------------------------------------------------------
lat <- data.frame(s = c(iconv("café", "UTF-8", "latin1"), "plain", "naïve"), stringsAsFactors = FALSE)
Encoding(lat$s[3]) <- "bytes"
saveRDS(lat, here("latin1.rds"))

# ---- an .rds holding a list of data frames ---------------------------------------
saveRDS(list(first = data.frame(a = 1:2), note = "text", second = data.frame(b = c("u", "v"))),
        here("list.rds"))
saveRDS(data.frame(a = integer(0), b = character(0)), here("empty.rds"))

# ---- the oracle ---------------------------------------------------------------------
num <- function(x) {
  vapply(x, function(v) {
    if (is.na(v) && !is.nan(v)) { t <- na_tag_of(v); if (nzchar(t)) paste0("NA(", t, ")") else NA_character_ }
    else if (is.nan(v)) "NaN" else if (is.infinite(v)) (if (v > 0) "Inf" else "-Inf")
    else sprintf("%.17g", v)
  }, character(1), USE.NAMES = FALSE)
}
describe_col <- function(x, name) {
  cls <- class(x)
  out <- list(name = name, class = if (is.null(attr(x, "class"))) character(0) else cls,
              typeof = typeof(x))
  if (inherits(x, "integer64")) {
    out$values <- i64_values
  } else if (is.factor(x)) {
    out$values <- as.integer(unclass(x)); out$levels <- levels(x)
  } else if (is.character(x)) {
    v <- unclass(x)
    # the oracle's copy of a "bytes" string (valid UTF-8 here) is marked UTF-8
    # so jsonlite writes it
    b <- which(Encoding(v) == "bytes")
    v[b] <- vapply(v[b], function(t) { Encoding(t) <- "UTF-8"; t }, "", USE.NAMES = FALSE)
    out$values <- enc2utf8(v)
  } else if (is.logical(x)) {
    out$values <- unclass(x)
  } else if (is.integer(x)) {
    out$values <- as.integer(unclass(x))
  } else {
    out$values <- num(unclass(x))
  }
  # exact = TRUE: attr() would otherwise take "labels" for a missing "label"
  for (a in c("label", "units", "tzone", "format.stata"))
    if (!is.null(attr(x, a, exact = TRUE))) out[[a]] <- attr(x, a, exact = TRUE)
  if (!is.null(attr(x, "labels"))) {
    lb <- attr(x, "labels")
    out$labels <- list(names = names(lb), values = if (is.character(lb)) unname(lb) else num(unname(lb)))
  }
  if (!is.null(attr(x, "na_values"))) {
    nv <- attr(x, "na_values"); out$na_values <- if (is.character(nv)) nv else num(nv)
  }
  if (!is.null(comment(x))) out$comment <- comment(x)
  out
}
describe_frame <- function(df, object = "") {
  keep <- !vapply(df, function(x) is.list(x) || !is.null(dim(x)) || is.complex(x), logical(1))
  cols <- lapply(which(keep), function(j) describe_col(df[[j]], names(df)[j]))
  rn <- attr(df, "row.names")
  list(object = object, nrow = nrow(df), names = names(df),
       rownames = if (is.character(rn)) rn else NULL,
       label = attr(df, "label", exact = TRUE), comment = comment(df), class = class(df),
       columns = unname(cols))
}
expected <- list(
  "frame.rds" = describe_frame(frame),
  "frame_v2.rds" = describe_frame(frame),
  "frame_none.rds" = describe_frame(frame),
  "workspace.RData:people" = describe_frame(people, "people"),
  "workspace.RData:towns" = describe_frame(towns, "towns"),
  "rownames.rds" = describe_frame(rn),
  "edge.rds" = describe_frame(edge),
  "latin1.rds" = describe_frame(lat),
  "list.rds:first" = describe_frame(list(a = 1:2) |> as.data.frame(), "first"),
  "empty.rds" = describe_frame(data.frame(a = integer(0), b = character(0))))
if (have_zstd) expected[["frame_zstd.rds"]] <- describe_frame(frame)
expected[["_about"]] <- list(r_version = paste(R.version$major, R.version$minor, sep = "."),
                             workspace_objects = c("f", "e", "people", "lst", "towns"))
writeLines(jsonlite::toJSON(expected, auto_unbox = FALSE, null = "null", na = "null",
                            digits = NA, pretty = TRUE), here("expected.json"), useBytes = TRUE)
cat("fixtures written; zstd:", have_zstd, "\n")
