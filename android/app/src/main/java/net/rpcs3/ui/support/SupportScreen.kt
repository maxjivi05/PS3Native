package net.rpcs3.ui.support

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.HelpOutline
import androidx.compose.material.icons.outlined.OpenInNew
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import net.rpcs3.R
import net.rpcs3.ui.components.PaneScaffold
import net.rpcs3.ui.components.PaneSectionTitle
import net.rpcs3.ui.components.PaneTab
import net.rpcs3.ui.theme.Dims
import net.rpcs3.ui.theme.Rpcs

private data class SupportLink(
    val iconRes: Int,
    val titleRes: Int,
    val subtitleRes: Int,
    val url: String
)

private val DonateLink = SupportLink(
    iconRes = R.drawable.ic_brand_paypal,
    titleRes = R.string.support_paypal,
    subtitleRes = R.string.support_paypal_desc,
    url = "https://paypal.me/MaxJividen"
)

private val CommunityLinks = listOf(
    SupportLink(
        iconRes = R.drawable.ic_brand_discord,
        titleRes = R.string.support_maxstechreview_discord,
        subtitleRes = R.string.support_maxstechreview_discord_desc,
        url = "https://discord.gg/eAwvAJhrkB"
    ),
    SupportLink(
        iconRes = R.drawable.ic_brand_reddit,
        titleRes = R.string.support_reddit,
        subtitleRes = R.string.support_reddit_desc,
        url = "https://www.reddit.com/r/EmulatorsForAndroid/"
    ),
    SupportLink(
        iconRes = R.drawable.ic_brand_youtube,
        titleRes = R.string.support_youtube,
        subtitleRes = R.string.support_youtube_desc,
        url = "https://youtube.com/@maxstechreview"
    )
)

@Composable
fun SupportScreen(
    modifier: Modifier = Modifier,
    onClose: (() -> Unit)? = null
) {
    val context = LocalContext.current
    var selected by remember { mutableIntStateOf(0) }

    val open: (String) -> Unit = { url ->
        runCatching { context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
    }

    val tabs = listOf(PaneTab(stringResource(R.string.support_tab), Icons.Outlined.HelpOutline))

    PaneScaffold(
        title = stringResource(R.string.support_title),
        tabs = tabs,
        selected = selected,
        onSelect = { selected = it },
        onBack = onClose,
        modifier = modifier
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            PaneSectionTitle(stringResource(R.string.support_donate_heading))
            SectionDescription(stringResource(R.string.support_donate_desc))
            SupportLinkRow(link = DonateLink, onOpen = open, highlighted = true)

            Spacer(Modifier.height(Dims.SectionSpacing))

            PaneSectionTitle(stringResource(R.string.support_heading))
            SectionDescription(stringResource(R.string.support_desc))
            CommunityLinks.forEachIndexed { index, link ->
                if (index > 0) {
                    Spacer(Modifier.height(Dims.RowSpacing))
                }
                SupportLinkRow(link = link, onOpen = open)
            }
        }
    }
}

@Composable
private fun SectionDescription(text: String) {
    Text(
        text = text,
        color = Rpcs.TextSecondary,
        fontSize = 12.sp,
        modifier = Modifier.padding(bottom = 10.dp)
    )
}

@Composable
private fun SupportLinkRow(
    link: SupportLink,
    onOpen: (String) -> Unit,
    highlighted: Boolean = false
) {
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val shape = RoundedCornerShape(Dims.CardCorner)
    val background = if (highlighted) Rpcs.Accent.copy(alpha = 0.08f) else Rpcs.SurfaceRaised
    val outline = when {
        focused -> Rpcs.FocusBorder
        highlighted -> Rpcs.Accent.copy(alpha = 0.35f)
        else -> Rpcs.Outline
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(background, shape)
            .border(if (focused) Dims.FocusBorderWidth else Dims.BorderWidth, outline, shape)
            .clickable(interactionSource = interaction, indication = null) { onOpen(link.url) }
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(
            modifier = Modifier
                .size(40.dp)
                .background(
                    Rpcs.AccentBright.copy(alpha = if (highlighted) 0.20f else 0.12f),
                    RoundedCornerShape(11.dp)
                ),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                painter = painterResource(link.iconRes),
                contentDescription = null,
                tint = Rpcs.AccentBright,
                modifier = Modifier.size(22.dp)
            )
        }
        Spacer(Modifier.width(14.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = stringResource(link.titleRes),
                color = Rpcs.TextPrimary,
                fontSize = 13.sp,
                fontWeight = FontWeight.Medium
            )
            Spacer(Modifier.height(2.dp))
            Text(
                text = stringResource(link.subtitleRes),
                color = Rpcs.TextSecondary,
                fontSize = 11.sp
            )
        }
        Icon(
            imageVector = Icons.Outlined.OpenInNew,
            contentDescription = null,
            tint = Rpcs.TextSecondary,
            modifier = Modifier.size(17.dp)
        )
    }
}
