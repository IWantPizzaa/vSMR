 = 'vSMR\\SMRRadar_TagDefinitions.cpp'
$text = [System.Text.RegularExpressions.Regex]::Replace($text, '(?m)^\s*"csid",\r?\n', '')
$text = [System.Text.RegularExpressions.Regex]::Replace($text, '(?m)^\s*previewMap\["csid"\] = "LAM1X";\r?\n', '')
$pattern = 'std::string CSMRRadar::NormalizeStructuredRuleSource\(const std::string& source\) const\s*\{.*?\n\}'
$replacement = @"
    std::string lowered = TrimAsciiWhitespace(source);
{
std::string CSMRRadar::NormalizeStructuredRuleSource(const std::string& source) const
