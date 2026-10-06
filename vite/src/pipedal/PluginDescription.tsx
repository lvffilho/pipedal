import { Remark } from 'react-remark';
import Typography from '@mui/material/Typography';

// Renders a plugin's markdown description. Split into its own module so the markdown
// stack is code-split out of the main bundle.
export default function PluginDescription(props: { description: string }) {
    return (
        <Remark
            rehypeReactOptions={{
                components: {
                    p: (props: any) => {
                        return (
                            <Typography variant="body2" paragraph={true} {...props} />
                        );
                    },
                    code: (props: any) => {
                        return (<code style={{ fontSize: 14 }} {...props} />);
                    },
                    a: (props: any) => {
                        return (
                            <a target="_blank" {...props} />
                        );
                    }
                },
            }}
        >
            {props.description}
        </Remark>
    );
}
